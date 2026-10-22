#pragma once

// The normalization registry (ADR 0050 R4): every field a comparison masks,
// strips or reduces, each with the nondeterminism source that makes masking it
// correct. The comparator's masks are driven FROM this table (the handshake
// dynamic fields and reductions, the initial-body routes, the stable-body
// prefixes, the first-order exclusions, the grouping lengths and boundaries),
// and every other masking site asserts its entry at compile time
// (static_assert(has_normalization("...")) beside the code), so a mask cannot
// exist without a registered reason. opennova-parity also writes the table into
// every verdict. A mask whose finding is real data keeps that data visible
// through another metric (scoreboard_cadence, frame_self_offset, the C:0x09 /
// S:0x2c milestones, handshake_key_lcg).

#include <cstdint>
#include <string_view>

namespace opennova::parity {

enum class MaskKind : uint8_t {
	HandshakeField, // a 0x41/0x42/0x81/0x82 flat-TLV field's value: `opcode`, `name`
	HandshakeCu, // a CU chunk's value inside a handshake: `opcode`, the CU `name`
	HandshakeCuLow16, // a CU chunk's decimal value reduced to its low 16 bits
	HandshakeLength, // a field's size reduced to the class {bytes, bytes + 1}
	InitialBody, // an initial-stream body left out of byte equality: `dir`, `identity`
	InitialSet, // an initial-stream identity whose distinct bodies compare as a set
	InitialRecords, // an initial-stream identity compared as decoded records by slot
	LengthExempt, // an identity left out of the length set (judged by another metric)
	BodyPrefix, // the first `bytes` of a stable body: `dir`, `identity`
	FirstOrder, // an identity left out of the first-occurrence order: `dir`, `identity`
	GroupLength, // a record whose length a packet-grouping key renders as `len=*`
	GroupReply, // a reply a steady gameplay packet's grouping key leaves out
	GroupBoundary, // a direction whose initial grouping ignores packet boundaries
	Field, // a decoded field the metrics reduce (named by `id` at the use site)
};

struct Normalization {
	const char *id;
	MaskKind kind;
	char dir; // 'C', 'S', or '*' (either direction / not directional)
	uint16_t selector; // the handshake opcode, or the 9-bit message identity
	const char *name; // the TLV / CU field name, or the decoded field
	uint32_t bytes; // BodyPrefix: the stripped prefix length; HandshakeLength: the class base;
	                // HandshakeField: 1 when the field's size follows its masked value
	const char *reason;
};

// A message identity: the dispatch tag plus the settings-table bit.
constexpr uint16_t identity_of(uint8_t tag, bool settings) {
	return static_cast<uint16_t>(tag | (settings ? 0x100u : 0u));
}

inline constexpr Normalization kNormalizations[] = {
	// --- the outer handshake (the retired verifier's dynamic field lists). The 0x41's
	// EIP/EPN and their 0x81 echo are NOT masked: they are the announce's own
	// destination (NapiNPSession_SendAnnouncePacket @0x61fcf5..0x61fd47), the
	// broadcast -1 and the enumerated host port on a LAN browse
	// (CNapiGameSession_InitTransportConnection @0x4c9eab/@0x4c9ec5..0x4c9ed1),
	// identical run to run.
	{ "handshake.0x41.AP", MaskKind::HandshakeField, 'C', 0x41, "AP", 1,
			"the game protocol's AP is the running executable's file name: CNapiNetwork_Init copies "
			"Napi_GetExeBasename's result (the call @0x4ca747, the copy @0x4ca759) and the 0x41 announce (NapiNPSession_SendAnnouncePacket @0x61fb3a) "
			"reads that copy, so each run names its own program (Jointops.exe on retail, the "
			"runtime's executable on ours, D-NET-NEW-L9-2); no host compares it (the identity "
			"checks read NVS, PN, PG and PV1). Its size follows the name" },
	{ "handshake.0x42.AP", MaskKind::HandshakeField, 'C', 0x42, "AP", 1,
			"the game protocol's AP is the running executable's file name: CNapiNetwork_Init copies "
			"Napi_GetExeBasename's result (the call @0x4ca747, the copy @0x4ca759) and the 0x42 join (CNapiNPConnection_SendClientJoin @0x61ff52) "
			"reads that copy, so each run names its own program (Jointops.exe on retail, the "
			"runtime's executable on ours, D-NET-NEW-L9-2); no host compares it (the identity "
			"checks read NVS, PN, PG and PV1). Its size follows the name" },
	{ "handshake.0x81.AP", MaskKind::HandshakeField, 'S', 0x81, "AP", 1,
			"the game protocol's AP is the running executable's file name: CNapiNetwork_Init copies "
			"Napi_GetExeBasename's result (the call @0x4ca747, the copy @0x4ca759) and a host's 0x81 (NapiNPProtocol_SendServerInfoPacket @0x6205ea) "
			"reads that copy, so each run names its own program (Jointops.exe on retail, the "
			"runtime's executable on ours, D-NET-NEW-L9-2); no host compares it (the identity "
			"checks read NVS, PN, PG and PV1). Its size follows the name" },
	{ "handshake.0x41.CI", MaskKind::HandshakeField, 'C', 0x41, "CI", 0,
			"the client's connection index, assigned per process" },
	{ "handshake.0x42.CI", MaskKind::HandshakeField, 'C', 0x42, "CI", 0,
			"the client's connection index, assigned per process" },
	{ "handshake.0x42.HK", MaskKind::HandshakeField, 'C', 0x42, "HK", 0,
			"the host key the 0x81 carried, drawn per host process" },
	{ "handshake.0x42.CK", MaskKind::HandshakeField, 'C', 0x42, "CK", 0,
			"the client key, drawn per session" },
	{ "handshake.0x42.SIP", MaskKind::HandshakeField, 'C', 0x42, "SIP", 0,
			"the server address as the client resolved it (adapter address)" },
	{ "handshake.0x42.SPN", MaskKind::HandshakeField, 'C', 0x42, "SPN", 0,
			"the server port as the client resolved it" },
	{ "handshake.0x42.SCRK", MaskKind::HandshakeField, 'C', 0x42, "SCRK", 0,
			"the client's session crypt key, drawn per session from the manager LCG "
			"(CNapiNPConnection_GenerateTxKey @0x61dfe0, NapiPRNG_NextInRange @0x62e450); "
			"its shape is handshake_key_lcg's" },
	{ "handshake.0x42.SCRK.length", MaskKind::HandshakeLength, 'C', 0x42, "SCRK", 62,
			"63 LCG draws less the NUL-slot picks: 61 or 62 characters plus the NUL, by "
			"where the session's draw falls in the stream (GenerateTxKey @0x61dfe0, "
			"NextInRange @0x62e450)" },
	{ "handshake.0x81.CI", MaskKind::HandshakeField, 'S', 0x81, "CI", 0,
			"the connection index the host echoes, assigned per process" },
	{ "handshake.0x81.UT", MaskKind::HandshakeField, 'S', 0x81, "UT", 0,
			"the host's uptime in milliseconds (GetTickCount since its start)" },
	{ "handshake.0x81.HK", MaskKind::HandshakeField, 'S', 0x81, "HK", 0,
			"the host key, drawn per host process" },
	{ "handshake.0x81.RIP", MaskKind::HandshakeField, 'S', 0x81, "RIP", 0,
			"the client's reflected IP (adapter address)" },
	{ "handshake.0x81.RPN", MaskKind::HandshakeField, 'S', 0x81, "RPN", 0,
			"the client's reflected port: an ephemeral UDP port" },
	{ "handshake.0x81.SUS1", MaskKind::HandshakeField, 'S', 0x81, "SUS1", 0,
			"the unique session id (GSID with a timestamp and hash), minted per session" },
	{ "handshake.0x82.CI", MaskKind::HandshakeField, 'S', 0x82, "CI", 0,
			"the connection index, assigned per process" },
	{ "handshake.0x82.MI", MaskKind::HandshakeField, 'S', 0x82, "MI", 0,
			"the member index the host assigns at admission" },
	{ "handshake.0x82.CK", MaskKind::HandshakeField, 'S', 0x82, "CK", 0,
			"the echo of the per-session client key" },
	{ "handshake.0x82.SK", MaskKind::HandshakeField, 'S', 0x82, "SK", 0,
			"the server key, drawn per session" },
	{ "handshake.0x82.SCRK", MaskKind::HandshakeField, 'S', 0x82, "SCRK", 0,
			"the server's session crypt key, drawn per session from the manager LCG "
			"(GenerateTxKey @0x61dfe0, NextInRange @0x62e450); its shape is "
			"handshake_key_lcg's" },
	{ "handshake.0x82.SCRK.length", MaskKind::HandshakeLength, 'S', 0x82, "SCRK", 62,
			"63 LCG draws less the NUL-slot picks: 61 or 62 characters plus the NUL, by "
			"where the session's draw falls in the stream (GenerateTxKey @0x61dfe0, "
			"NextInRange @0x62e450)" },
	{ "handshake.0x82.RIP", MaskKind::HandshakeField, 'S', 0x82, "RIP", 0,
			"the client's reflected IP (adapter address)" },
	{ "handshake.0x82.RPN", MaskKind::HandshakeField, 'S', 0x82, "RPN", 0,
			"the client's reflected port: an ephemeral UDP port" },
	{ "handshake.0x42.CU.UdpCode1", MaskKind::HandshakeCu, 'C', 0x42, "UdpCode1", 0,
			"a per-session UDP reachability code the client draws" },
	{ "handshake.0x42.CU.UdpCode2", MaskKind::HandshakeCu, 'C', 0x42, "UdpCode2", 0,
			"a per-session UDP reachability code the client draws" },
	{ "handshake.0x42.CU.CI0", MaskKind::HandshakeCuLow16, 'C', 0x42, "CI0", 0,
			"CNapiServerInfo_SerializeToSession stores the u16 character id into the low "
			"word of an uninitialized stack dword and prints the whole dword "
			"(@0x4c3a27/@0x4c3a2c); the host keeps the low u16 "
			"(NapiNetConfig_LoadFromConnTags @0x4c7260)" },
	{ "handshake.0x42.CU.CI1", MaskKind::HandshakeCuLow16, 'C', 0x42, "CI1", 0,
			"CNapiServerInfo_SerializeToSession stores the u16 character id into the low "
			"word of an uninitialized stack dword and prints the whole dword "
			"(@0x4c3a80/@0x4c3a85); the host keeps the low u16 "
			"(NapiNetConfig_LoadFromConnTags @0x4c7260)" },
	{ "handshake.0x82.CU.NWUID", MaskKind::HandshakeCu, 'S', 0x82, "NWUID", 0,
			"the NovaWorld user id the host mints per connection" },

	// --- initial-stream bodies routed to stateful checks instead of byte equality
	{ "initial.C.0x02", MaskKind::InitialBody, 'C', 0x002, "JOIN_PADDING_ECHO", 0,
			"echoes the host's per-session S2C 0x02 join padding probe (§5.55)" },
	{ "initial.C.0x08", MaskKind::InitialBody, 'C', 0x008, "TIME_SYNC_REPLY", 0,
			"the echoed sequence and the client's GetTickCount (§5.34): a wall clock" },
	{ "initial.C.0x1c", MaskKind::InitialBody, 'C', 0x01C, "CHARATTR_CRC_REPLY", 0,
			"answers the host's per-session S2C 0x39 charattr CRC seed" },
	{ "initial.C.0x20", MaskKind::InitialBody, 'C', 0x020, "ENTITY_CHECKSUM_REPLY", 0,
			"the checksum is XORed with S2C 0x30's per-request key (integrity metrics)" },
	{ "initial.C.0x21", MaskKind::InitialBody, 'C', 0x021, "CHECKSUM_REPLY", 0,
			"the ammo CRC is XORed with S2C 0x31's per-request key (integrity metrics)" },
	{ "initial.C.0x28", MaskKind::InitialBody, 'C', 0x028, "LOADOUT_REQUEST", 0,
			"the kill-list request carries the join window's tick bounds (§5.33)" },
	{ "initial.C.0x2c", MaskKind::InitialBody, 'C', 0x02C, "RTT_PING_PONG", 0,
			"[u32 GetTickCount][u8 1] ping (Client_ProcessNetworkFrame @0x42c41c) or [u32 "
			"echoed S2C 0x57 stamp][u8 0] pong (NapiNPClientMsg_0x057_RTT @0x432210): both "
			"stamps are wall clocks (the host's @0x51e417), and how many pongs precede the "
			"first ping follows the host's 0x57 cadence against the deploy time; the "
			"rtt_echo metric checks the pongs" },
	{ "initial.C.0x3d", MaskKind::InitialBody, 'C', 0x03D, "LOADED_MODEL_PAGE_REPLY", 0,
			"answers S2C 0x68, whose page cursor advances on a process-global wrap" },
	{ "initial.S.0x02", MaskKind::InitialBody, 'S', 0x002, "JOIN_PADDING_PROBE", 0,
			"the §5.55 probe carries the host's per-session position ack" },
	{ "initial.S.0x19", MaskKind::InitialBody, 'S', 0x019, "SPAWN_ACK_TIMESTAMP", 0,
			"a live GetTickCount timestamp" },
	{ "initial.S.0x30", MaskKind::InitialBody, 'S', 0x030, "ENTITY_CHECKSUM_REQ", 0,
			"a per-request random key; rows and replies have the integrity metrics" },
	{ "initial.S.0x31", MaskKind::InitialBody, 'S', 0x031, "LOADOUT_CRC_REQ", 0,
			"a per-request random key; rows and replies have the integrity metrics" },
	{ "initial.S.0x39", MaskKind::InitialBody, 'S', 0x039, "CHARATTR_CRC_CHALLENGE", 0,
			"the per-session charattr CRC seed" },
	{ "initial.S.0x43", MaskKind::InitialBody, 'S', 0x043, "TIME_SYNC_PING", 0,
			"the host's server timestamp (§5.34)" },
	{ "initial.S.0x68", MaskKind::InitialBody, 'S', 0x068, "LOADED_MODEL_PAGE_REQUEST", 0,
			"the page cursor advances on a process-global wrap" },
	{ "initial.S.0x1a", MaskKind::InitialBody, 'S', 0x01A, "WAIT_FOR_GAME_START_ACK", 0,
			"the game-start gate's body is the host's GetTickCount (NetPacket_WriteTimestamp "
			"@0x5046c0, the tick @0x5046d3), which the client keeps as the kill-list window's "
			"max (NapiNPClientMsg_0x01A @0x425ec0): a wall clock" },
	{ "body.S.0x64.session_block", MaskKind::Field, 'S', 0x064, "seed / session id / token", 0,
			"S2C 0x64 chunks the 180-byte session block (NetPacket_WriteFixed180Block "
			"@0x507300, the copy from 0x24D1E08 @0x507379) that Game_StartMission fills per "
			"mission (CNapiGameSession_InitRandomSeedOrRequest @0x51e8f0, the call @0x5248a7): "
			"block bytes 0..31 are GetTickCount x rand() (@0x51e91f), 32..35 the session id "
			"(u16)(GetTickCount x GetTickCount) (@0x51e93f), 148..179 a second GetTickCount x "
			"rand() token (@0x51e9a2); those bytes are zeroed, the header, the rules word and "
			"the three names compare" },
	{ "initial.S.0x16", MaskKind::InitialSet, 'S', 0x016, "PLAYER_LIST", 0,
			"the scoreboard rides the 311-tick integrity boundary (Server_TickUpdate "
			"@0x51D90D), whose phase against the join varies per run: how many one-row lists "
			"precede the two-row one is timing, so the initial window's distinct bodies "
			"compare as a set" },
	{ "initial.S.0x46", MaskKind::InitialSet, 'S', 0x046, "PLAYER_SYNC", 0,
			"a joiner's first 0x46 answers the C2S 0x22 its first 0x16 provokes "
			"(NapiNPServerMsg_0x022 @0x514C90), so how many 0x46 the initial window holds "
			"is the 0x16 phase: the decoded bodies compare as a set" },
	{ "initial.S.0x46.timer_score", MaskKind::Field, 'S', 0x046, "0x0800 / 0x0020", 0,
			"the 0x0800 dword is the slot player's NovaWorld squad id (net config +0x1A4) "
			"and the 0x0020 byte its NovaWorld session byte (NapiNPPlayer+0x9C) "
			"(NetPacket_SerializePlayerSync0x46 @0x50613b..0x50618b, @0x506246/@0x506257): "
			"per-account session identity, not case data" },
	{ "initial.S.0x0c", MaskKind::InitialRecords, 'S', 0x00C, "ENTITY_SPAWN_BATCH", 0,
			"pages of pool-0 organic records: compared record by record (keyed by slot) "
			"with the live poses reduced (initial.S.0x0c.live_pose)" },
	{ "initial.S.0x0c.live_pose", MaskKind::Field, 'S', 0x00C, "live record pose", 0,
			"a visible pool-0 body (record minimapFlags bit 0 clear: a deployed player, an "
			"AI organic) carries its live position and heading, which its owner's input or "
			"AI think moves between the add and the send; a hidden body (bit 0 set) sits "
			"at its placement and compares exactly" },
	{ "initial.S.0x0d", MaskKind::InitialRecords, 'S', 0x00D, "POOL_SPAWN", 0,
			"pages of pool spawn records: compared record by record (keyed by slot) with the "
			"live poses left out (initial.S.0x0d.live_pose)" },
	{ "initial.S.0x0d.live_pose", MaskKind::Field, 'S', 0x00D, "live record pose", 0,
			"a record whose physics runs every tick carries its pose (x, y, z, euler z/x/y) at "
			"the tick the join's 0x0D snapshot is written, which follows the joiner's own "
			"message timing: a hull on water (entity+0x24 bit 0x8000) steps a heave phase into "
			"its four suspension forces every tick (Entity_ProcessPlatformPhysics @0x4839CE, "
			"the arm @0x4839FA..0x483A07, Entity_ApplyBreathingOscillation @0x463BE0 called "
			"@0x483A1D, the phase step @0x463BEB); a vehicle with Flags bit 0x2000 or 0x40 or an occupant (+0x170, the "
			"record's parent) never takes the sleep return "
			"(Entity_ProcessWheeledVehiclePhysics @0x475EB3..0x475EC7, @0x475F17; "
			"Entity_ProcessTrackedVehiclePhysics @0x47C2A5..0x47C2B7, @0x47C308); a record "
			"carried on a live one (+0x28, the record's target) takes its pose from its carrier "
			"every update (Entity_UpdateTransformAndTurret @0x440CBF, @0x44109D..0x4410DD, "
			"@0x441153..0x441177). One live by its own physics also leaves out its contact bit "
			"(0x2000, set by contact: Entity_ProcessTrackedVehiclePhysics @0x47E57B) and its "
			"ground link (the target), which each mover re-probes "
			"(Entity_RaycastGroundHeightAndObject's store @0x414370, called from "
			"Entity_UpdateVehiclePhysics @0x48AFD1, Entity_UpdateWatercraftPhysics @0x48D535 "
			"and the other movers). Liveness is RR's record's; every other field compares, and "
			"the steady window still measures the vehicles (state.position)" },
	{ "initial.S.0x10", MaskKind::InitialRecords, 'S', 0x010, "STATIC_ENTITY_BATCH", 0,
			"pages of pool-2 static records: compared record by record (keyed by slot) "
			"with the zone block stripped (world.S.0x10.zone_prefix)" },
	{ "world.S.0x10.zone_prefix", MaskKind::Field, 'S', 0x010, "zone byte / radius", 0,
			"a page's statics before its first zone-chain hit stream the serializer's "
			"uninitialized stack outs (NetPacket_SerializePool2StaticToBuffer @0x5042F0; "
			"ZoneSlotChain_GetZoneInfo writes only on a hit @0x4A27D1/@0x4A27DC), so which "
			"statics carry a byte and radius varies run to run (D-NET-394)" },

	// --- length sets judged by another metric
	{ "length.S.0x10", MaskKind::LengthExempt, 'S', 0x010, "STATIC_ENTITY_BATCH", 0,
			"each 0x10 record's trailing zone block is retail stack residue "
			"(world.S.0x10.zone_prefix), so the page lengths follow it; the records compare "
			"by slot" },
	{ "length.S.0x0d", MaskKind::LengthExempt, 'S', 0x00D, "POOL_SPAWN", 0,
			"a live record's value-gated fields follow its physics tick: the serializer writes "
			"each euler angle only when nonzero (NetPacket_SerializeEntityPoolToPacket_0 "
			"@0x503B3A..0x503B74) and an awake vehicle's ground link (the target) only while "
			"its mover's probe hits (Entity_RaycastGroundHeightAndObject @0x414370), so the "
			"page lengths follow the join's tick (initial.S.0x0d.live_pose); the records "
			"compare by slot" },
	{ "length.C.0x3d", MaskKind::LengthExempt, 'C', 0x03D, "LOADED_MODEL_PAGE_REPLY", 0,
			"a reply's length is 4 + 4 * min(50, count - start) "
			"(NetPacket_WriteEntityIndexList @0x42d9c3) for the start the host's "
			"process-global page cursor asks: compared per start (reply.C.0x3d.start)" },
	{ "reply.C.0x3d.start", MaskKind::Field, 'C', 0x03D, "start_index", 0,
			"which pages fall inside the window is the host's page cadence: a reply's "
			"length compares only for a start both runs answered" },

	// --- stable bodies
	{ "stable.S.0x0f.tick", MaskKind::BodyPrefix, 'S', 0x00F, "WORLD_STATE_LOAD", 4,
			"S2C 0x0F opens with the live session tick; the rest is the same-case pose and "
			"flags" },

	// --- first-occurrence order
	{ "order.S.0x30", MaskKind::FirstOrder, 'S', 0x030, "ENTITY_CHECKSUM_REQ", 0,
			"0x30/0x31 alternate on one process-global phase, so a join can enter either half" },
	{ "order.S.0x31", MaskKind::FirstOrder, 'S', 0x031, "LOADOUT_CRC_REQ", 0,
			"0x30/0x31 alternate on one process-global phase, so a join can enter either half" },
	{ "order.S.0x16", MaskKind::FirstOrder, 'S', 0x016, "PLAYER_LIST", 0,
			"the scoreboard rides the process-global 311-tick g_ScoreboardBroadcastTimer "
			"(Server_TickUpdate @0x51D8F9..0x51D917, reset per mission by Nbstat_StartupInit "
			"@0x4FDE41): a join meets its first list at the join's phase against the mission "
			"start; scoreboard_cadence keeps an off-clock list visible" },
	{ "order.S.0x46", MaskKind::FirstOrder, 'S', 0x046, "PLAYER_SYNC", 0,
			"a joiner's first 0x46 answers the C2S 0x22 that its first 0x16's unbound-slot "
			"row provokes (NapiNPServerMsg_0x022 @0x514C90), so it inherits the 0x16 phase" },
	{ "order.S.0x79", MaskKind::FirstOrder, 'S', 0x079, "NETWORK_QUALITY", 0,
			"the network-quality broadcast rides its own 0x136-tick countdown: its first send "
			"is a timer phase against the joiner's reply round trips" },
	{ "order.S.0x6e", MaskKind::FirstOrder, 'S', 0x06E, "SPAWN_WAVE_STATUS", 0,
			"a 1 Hz periodic-second pass: its first send races the joiner's 0x28/0x23 round "
			"trip" },
	{ "order.S.0x6f", MaskKind::FirstOrder, 'S', 0x06F, "ZONE_TIMER_VALUE", 0,
			"the 1 Hz Server_UpdateCaptureZoneEntities pass (@0x519690): its first send races "
			"the joiner's 0x28/0x23 round trip" },
	{ "order.S.0x40", MaskKind::FirstOrder, 'S', 0x040, "OVERLAY_STATE", 0,
			"a joiner's first 0x40s are the tick head's overlay walk (Server_TickUpdate: "
			"Server_UpdateAllActivePlayerSlots @0x518820 called @0x51D88B, its visit calling "
			"Server_BuildOverlayStateForPlayer @0x517FC0 (the call @0x5188D6)), which runs "
			"ahead of the receive "
			"pump that takes the C2S 0x0B (@0x51D895; Server_OnPlayerJoin's bundle ends with "
			"its 0x3E @0x51AAEE), and the per-slot block sends the 0x0A / 0x57 behind the "
			"connection's send holdoff (@0x51E3D6): a bundle that waits for a boundary leaves "
			"with the next tick's walk 0x40s ahead of that block's 0x0A / 0x57, one that leaves "
			"at once has the block's 0x0A / 0x57 first and the 0x40s a boundary later. x6's "
			"01tr-coop-aikill RR-aa (5.4 ms from the 0x0B to the bundle) and tdh-i3a-tdm-late "
			"RR's late joiner (9.5 ms) carry the 0x57 ahead of the 0x40, their partners (180 "
			"and 195 ms) the 0x40 first" },
	{ "order.C.0x20", MaskKind::FirstOrder, 'C', 0x020, "ENTITY_CHECKSUM_REPLY", 0,
			"answers S2C 0x30, whose half of the 0x30/0x31 alternation is process-global" },
	{ "order.C.0x21", MaskKind::FirstOrder, 'C', 0x021, "CHECKSUM_REPLY", 0,
			"answers S2C 0x31, whose half of the 0x30/0x31 alternation is process-global" },
	{ "order.C.0x22.phase", MaskKind::Field, 'C', 0x022, "PLAYER_SYNC_REQUEST", 0,
			"the joiner re-requests every S2C 0x16 row whose slot its roster has not bound with "
			"a reliable C2S 0x22 (NapiNPClientMsg_PlayerList @0x42FAE0, the queue call "
			"@0x42FC35), so the 0x22 inherits the 0x16's scoreboard phase (order.S.0x16): where "
			"it lands against the C2S 0x0A answering the S2C 0x11 is that phase, unless the "
			"0x16 rode the 0x11's own packet. x5's retail joiners order the two both ways with "
			"the 0x16 apart (cp04-objcoop-deploy RR 0x22 first, RR-aa 0x0A first) and send "
			"them in one packet, the 0x22 first, every time it rode along (RR-aa on ten cases, "
			"RR and OR on four each). A pair where either run's first 0x11 packet carries no "
			"0x16 leaves the 0x22 out of the C2S first-occurrence order and initial grouping" },

	// --- packet grouping keys
	{ "group.S.0x0a.length", MaskKind::GroupLength, 'S', 0x00A, "PER_FRAME_UPDATE", 0,
			"a S2C 0x0A's length follows its budget-capped contents (g_EntitySendBudget, "
			"NetPacket_SerializeEntityStatesToPacket @0x50F070) and the free-running phase's "
			"sub-block (NetPacket_WritePlayerState @0x4FF6B0), so a grouping keys its "
			"identity and flags, not its length" },
	{ "group.C.0x0c.length", MaskKind::GroupLength, 'C', 0x00C, "ENTITY_UPLINK", 0,
			"a C2S 0x0C's length follows the uplink's live contents, so a grouping keys its "
			"identity and flags, not its length" },
	{ "group.S.0x0c.length", MaskKind::GroupLength, 'S', 0x00C, "ENTITY_SPAWN_BATCH", 0,
			"a world-stream page's length is its records' (judged by initial_records); where "
			"retail's 650-byte page cut falls follows them" },
	{ "group.S.0x0d.length", MaskKind::GroupLength, 'S', 0x00D, "POOL_SPAWN", 0,
			"a world-stream page's length is its records' value-gated fields (judged by "
			"initial_records): where the page cut falls follows them" },
	{ "group.S.0x10.length", MaskKind::GroupLength, 'S', 0x010, "STATIC_ENTITY_BATCH", 0,
			"a world-stream page's length carries retail's uninitialized zone blocks "
			"(world.S.0x10.zone_prefix)" },
	{ "group.S.0x10.run", MaskKind::Field, 'S', 0x010, "page count and boundaries", 0,
			"the pool-2 static stream's records carry retail's uninitialized zone blocks "
			"(world.S.0x10.zone_prefix, D-NET-394), whose bytes decide whether the last statics "
			"spill into one more S2C 0x10 message (x5's 01tr-coop-drive A/A: RR 53 messages of "
			"33150 bytes, RR-aa 54 of 33170), so how many groups the stream takes and where its "
			"last page shares a packet follow that residue. The 0x10 pages leave the grouping "
			"key, and the stream stands as one element at its first page; its records compare "
			"by slot (initial_records)" },
	{ "group.S.0x20.length", MaskKind::GroupLength, 'S', 0x020, "POOL3_SYNC", 0,
			"a world-stream page's length is its records' value-gated fields (judged by "
			"length_set and initial_body): where the page cut falls follows them" },
	{ "group.S.0x45.length", MaskKind::GroupLength, 'S', 0x045, "TERRAIN_LOAD", 0,
			"a world-stream page's length is its records' (judged by length_set and "
			"initial_body): where the page cut falls follows them" },
	{ "group.fragment_length", MaskKind::Field, '*', 0, "fragment length", 0,
			"a fragmented record (FRAG_CONT 0x04 / FRAG_END 0x02, CNapiNPConnection_"
			"DispatchMessage @0x622570) is split at the packet's remaining room, so its length "
			"is the records before it (a phase-bound 0x16 in front shortens it); the "
			"reassembled message's length and body are judged on its event" },
	{ "group.length_exempt", MaskKind::Field, '*', 0, "length and length-field size", 0,
			"a length-exempt identity's record length (length.*) follows what its length "
			"set leaves out, and the encoder sizes the length field from the payload length "
			"(LEN8 0x20 below 256 bytes, else LEN16 0x40: NapiNPMessage_Create "
			"@0x628304..0x62831B), so a grouping keys neither: c2's ASH last S2C 0x10 page is "
			"290 B (0x40) on retail against 238 B (0x20) on ours, the residue's radius words" },
	{ "group.S.0x57.reply", MaskKind::GroupReply, 'S', 0x057, "RTT_ECHO", 0,
			"the host answers each client ping (queued every client frame, "
			"Client_ProcessNetworkFrame @0x42c41c) from its receive pump, and the reply rides "
			"the next boundary's packet: how many ride with the gameplay record follows the "
			"client's frame rate against the host's boundary. A steady gameplay packet keys "
			"without them; a packet of replies alone keeps them" },
	{ "group.S.0x57.order", MaskKind::Field, 'S', 0x057, "RTT_ECHO", 0,
			"steady_order_distribution leaves the RTT replies out of its record sequence too: "
			"the host queues the tick's slot walk (Server_UpdateAllActivePlayerSlots, called "
			"@0x51D88B in Server_TickUpdate, the S2C 0x40 overlay among it) before its receive "
			"pump (CNapiNetwork_PumpServerProtocolRecv @0x51D895, the 0x57 answering a C2S "
			"0x2C) into one connection queue packed at the boundary, so whether a 0x57 rides "
			"ahead of or behind the 0x40 follows the tick its 0x2C arrived on against the tick "
			"the overlay was queued on, and the share follows where the joiner's 0x2C falls in "
			"the host's send window: one monotone curve for retail "
			"hosts and ours in x7 (L3-s7: the 0x2C's median ms after the host's previous packet "
			"against the 0x57-before-0x40 share, RR 156 / 172 / 171 at 5.7 / 4.0 / 5.7%, RR-aa "
			"171 / 94 / 172 at 3.7 / 8.8 / 5.8%, OR 57 / 19 / 119 at 13.1 / 14.6 / 10.1% on "
			"ash-capture / ash-deploy / cp04-deploy); over the producers alone x7's RR against "
			"RR-aa reads 0.00..0.15 units on every case" },
	{ "group.steady_order", MaskKind::Field, '*', 0, "record order within a packet", 0,
			"a reply the receive pump queues (S2C 0x57 for a C2S 0x2C, C2S 0x2C for a S2C "
			"0x57) lands before or after the tick's own records by when its request arrived: "
			"the connection queue (CNapiNPConnection_QueueMessage @0x628640) is packed in "
			"queue order at the boundary (BuildOutgoingPackets @0x628430). A steady packet "
			"groups as its record multiset; steady_order_distribution judges the order of "
			"its producers' records (group.S.0x57.order)" },
	{ "initial.S.0x5a.reply", MaskKind::Field, 'S', 0x05A, "loadout reply boundary", 0,
			"the host answers each C2S 0x2F loadout submit with an S2C 0x5A "
			"(NapiNPServerMsg_HandlePlayerLoadout @0x515790, Server_SendWeaponSlotListToPlayer "
			"called @0x515F95, its send @0x502943) on its next boundary, so whether a joiner's "
			"two 0x5A share a packet follows how its send pump batched the two 0x2F "
			"(initial.C.pump_boundary): x5's 03tr-dm RR joiner sent them 3.1 s apart and the "
			"0x5A answering the lone first one left alone, RR-aa sent both in one packet and "
			"got both together. Each 0x5A stands as its own element of the S2C initial "
			"grouping, ahead of its packet's other records" },
	{ "initial.S.join_tail", MaskKind::Field, 'S', 0x00A, "join packet tail", 0,
			"the S2C initial window closes on the packet that carries the join bundle "
			"(Server_OnPlayerJoin @0x51A680, its last send the 0x3E @0x51AAEE), whose own "
			"0x0A opens the gameplay stream; what rides behind the bundle is the 0x0B's "
			"arrival against the send holdoff (@0x51E3D6): the next tick's overlay 0x40s "
			"(order.S.0x40) when the bundle waited for a boundary, the per-slot block's own "
			"0x0A when it left in the same frame (x6's 01tr-coop-aikill: RR's packet ends "
			"[0x3E, 0x40] 180 ms after the 0x0B, RR-aa's [0x3E, 0x0A] 5.4 ms after). That "
			"packet keys its 0x0A once" },
	{ "steady.distribution_floor", MaskKind::Field, '*', 0, "share distribution samples", 0,
			"a share distribution's tolerance unit is max(0.08, RR share / 2): one sample moves a "
			"key's share by 1 / n, so under 13 samples (1 / 13 < 0.08) one sample's phase alone "
			"crosses a unit. x5's kick (11 to 14 steady packets before the punt) read 1.21 units "
			"between RR and RR-aa on S2C. A distribution compares at 13 samples or more on both "
			"sides, and one side under it skips the row whichever run it is: how many steady "
			"packets precede a punt is its phase, the retail runs of x4..x6's kick holding 10 "
			"to 14 on C2S and S2C alike (x4 RR 10 against RR-aa 13 on C2S, x5 RR 11 against "
			"RR-aa 14 on S2C), so a subject's 10 to 12 against an RR at 13 (x6) is retail's "
			"own range; a subject with no steady gameplay still fails gameplay_cadence" },
	{ "initial.C.pump_boundary", MaskKind::GroupBoundary, 'C', 0, "group boundary", 0,
			"the per-frame send pump (PumpClientProtocolSend @0x42c4bc) flushes what each "
			"frame queued; whether the game-start 0x4E (@0x42e1d9) and Game_StartMission's "
			"entry legs (@0x524400/@0x524538/@0x524801) share a frame is load timing, so "
			"the initial C2S stream compares as one record sequence" },

	// --- decoded fields the metrics reduce
	{ "trace.session", MaskKind::Field, '*', 0, "session", 0,
			"the client UDP port is ephemeral: each run binds its one gameplay session" },
	{ "trace.ts_ns", MaskKind::Field, '*', 0, "ts_ns", 0,
			"capture timestamps are wall clock: only rates, gaps and windows compare" },
	{ "window.tag_common_length", MaskKind::Field, '*', 0, "tag window", 0,
			"how long a run's comparison interval is (the session's first packet to the "
			"steady end) follows the runner's readiness detection, not the wire (c1's 03TR "
			"OR ran 12 s longer than RR): the tag histogram takes each run's first packet "
			"plus the shorter interval of the pair, so a stream on the mission clock (03TR's "
			"0x40 overlay bursts every 1792 ticks) is not counted over different lengths" },
	{ "initial.S.spawn_pump_boundary", MaskKind::Field, 'S', 0, "initial packet boundary", 0,
			"the host admits a joiner in two legs: the reply to its C2S 0x02, then the spawn "
			"pump's add (S2C 0x03, the add's settings records, 0x05, 0x04, 0x7B: "
			"CNapiServer_ProcessPendingPlayerSpawns @0x4C9034..0x4C9111), which runs only in "
			"the periodic second (Server_TickUpdate's g_PeriodicSecondTimer @0x51DB6D..0x51DB93, "
			"the pump @0x51DBFD), a phase draw in [0, 1 s) after the admission, so the two share "
			"a packet or not (x1 fragrespawn: RR 0.595 s apart, RO the same packet): the add "
			"opens its own initial group" },
	{ "window.tag_stream_anchor", MaskKind::Field, '*', 0, "stream count span", 0,
			"a joiner's join (its C2S 0x0B: NapiNPServerMsg_PlayerJoinRequest @0x51AB10 calls "
			"Server_OnPlayerJoin @0x51A680 (the call @0x51ABA1), slot state 6 @0x51A712) lands "
			"at a different offset "
			"from each run's first packet (the joiner's load: x1 kick RR joined 34.6 s after its "
			"hello, RO 24.2 s), while a stream of the slot opens a fixed time after it (the "
			"overlay visit runs only for a state-6 slot, Server_UpdateAllActivePlayerSlots "
			"@0x518878, one visit every 14 ticks over 128 indices @0x5188CB..0x518906, 29 of "
			"which carry an S2C 0x40 entry on TDH_I3A): a stream of 20 or more counts from each "
			"run's join (its first C2S 0x0B, else the first S2C 0x0F) over the pair's shorter "
			"span from it, so a stream that starts late counts short (it fails once the "
			"lateness passes the count floor, about a fifth of the span); a timer-phase stream "
			"(one the first-occurrence order leaves out: retail draws only its first send's "
			"phase, within one period of the join, plus the joiner's 0x28 / 0x23 round trip "
			"for 0x6E / 0x6F) counts from the join too, about one occurrence short; a stream "
			"that reaches the joiner before its join (an S2C 0x16 during the load) keeps its "
			"own first" },
	{ "length.S.0x40.burst_tail", MaskKind::Field, 'S', 0x040, "multi-entry 0x40 length", 0,
			"the overlay visit queues 6-byte entries into a 16-entry buffer "
			"(Server_QueueOverlayStateEntry: the store @0x50FECA..0x50FEFF) that is flushed "
			"first when full (@0x50FEA0..0x50FEAE) and sent as [count][entries] "
			"(Server_SendPendingOverlayState @0x50FE20, the send @0x50FE94), so a burst is full "
			"97-byte 0x40s and a "
			"shorter last one. A deploy's visit (or a rejoin's after a rotation, whose slot keeps "
			"its index) stages every pool-2 entry plus that visit's own pool-1 entry when its row "
			"is populated, a phase draw (the visit index, Server_BuildOverlayStateForPlayer "
			"@0x518007), so that burst's last 0x40 carries one entry more or less: such a tail "
			"(the last 0x40 after a full one) one entry from a tail the other run holds is "
			"dropped. A join's visit is no draw: the index advances only in state 6 "
			"(@0x518878) and nothing at the join or the deploy writes it, so a fresh slot's "
			"join burst (the session's first) and every 0x40 outside a burst stay exact" },
	{ "window.rotation_load_gap", MaskKind::Field, 'B', 0, "E2 mission change", 0,
			"a host whose rotation plays its next mission inside the steady window (E2) "
			"pauses gameplay from the round end to the next mission: the round end sends "
			"each playing slot a zero 0x61 seed and the 0x1D board and moves it to state 7 "
			"(Server_ProcessRoundEnd @0x516790 / @0x516839 / @0x51685E), the host's 0x0A "
			"goes to state-6 slots only (Server_SendEntityStateToPlayer @0x517BD2), the "
			"joiner's 0x1D raises its round-over gate (@0x430858), which holds its 0x0C "
			"(Client_ProcessNetworkFrame @0x42C467); after the 2790-tick linger (@0x5166C4) "
			"the teardown queues each slot S2C 0x25 (@0x516385), which leaves at the first "
			"load pump whose send countdown is 0 (the head pump @0x5245E8 or another ahead of "
			"the terrain load @0x524F7C for a residual of 1 to 3, the first tick beside the "
			"load-end 0x7B for a late residual under holdoff 12), the joiner's reload "
			"answers with C2S 0x48 (Game_StartMission @0x524470), the host's load end sends "
			"0x7B (@0x526267) and the joiner's reload takes the next 0x0B. Retail's last "
			"sample before the pause comes within one send interval of the 0x1D (192 ms or "
			"less under holdoff 12), so a steady "
			"gap whose first sample lies within 500 ms of a round end's board (an S2C 0x1D, "
			"either side), or the window's tail when its last sample does, is the pause, "
			"not a stall; every round end in the window is excused the same way. A 0x25 "
			"that follows its round end's 0x1D by less than the linger is "
			"ROUND_LINGER_SHORT. The milestones take retail's order through the steady "
			"end: the round end's 0x61 and 0x1D, the 0x56 its board pull answers, the 0x25, "
			"the joiner's first 0x48 after it, the first 0x7B and 0x0B after the 0x25, the "
			"first 0x0A after that header; presence and order are two rows, each against "
			"its own A/A" },
	{ "window.session_end", MaskKind::Field, 'B', 0x103, "a scenario's own disconnect", 0,
			"a scenario whose play ends the joiner's session (its ends_joiner_session, E4's "
			"kick: a listen host's SERVER PUNT ALL) has the host queue the joiner the "
			"session disconnect, reason 33 and the tag S.C:SPA (Server_HandleBanPuntCommand "
			"@0x50B945..0x50B951), which reaches the joiner as the S2C 0x103 description "
			"record; its receipt moves the connection from state 5 to 6 "
			"(CNapiNPConnection_HandleDescriptionPacket, SetState(6) @0x621D5F), and "
			"leaving state 5 tears the connection down at once "
			"(CNapiNPConnection_SetState @0x626250, the teardown call @0x62628F), so no "
			"gameplay follows in either direction. Such a run's gameplay coverage ends at "
			"the first S2C 0x103 in the steady window; the flag is the run's scenario's, "
			"never read off the capture. Its rates (bandwidth, gameplay_cadence) count that "
			"covered play over its own length, since the window's rate is the punt's timing: "
			"x5's kick covered 1.08 to 1.29 s of a 45 s window, S2C 75 B/s on RR against 90 on "
			"RR-aa over the window and 3149 against 3451 over the play. A stream's count "
			"(window.tag_stream_anchor) stops at the same record, which lands a readiness wait "
			"after the join, so each run counts from its join to its own coverage end over that "
			"span's length: x6's kick joiner sent C2S 0x0C 78 times over RR's 15.26 s from the "
			"join to the punt and 73 over RR-aa's 14.36 s, 5 apart over a common span and 0.4 "
			"apart over each run's own" },
	{ "window.dead_joiner_uplink", MaskKind::Field, 'C', 0x00C, "uplink gap while dead", 0,
			"a joiner whose own body has no health sends no C2S 0x0C: the uplink build "
			"returns nothing on a zero health word or the dead flag "
			"(Player_BuildTag0CInputBody @0x42A550: the health word @0x42a58f, the flag "
			"@0x42a599). A gap or a window edge holding a S2C 0x0A whose own-player tail "
			"reports health 0 is that death, not a stall: x1's selfnade RR joiner stops "
			"uplinking at its death for the window's last 72 s" },
	{ "window.tag_presence_grace", MaskKind::Field, '*', 0, "tag window end", 0,
			"an event on the mission clock lands at a different offset from each run's "
			"join, so one near the window end falls inside one run's window and just past "
			"the other's (c1's 03TR RO 0x12, 0.5 s past): an identity is not missing when "
			"it lands within 10 s past that run's window end" },
	{ "trace.read_time", MaskKind::Field, '*', 0, "ts_ns (hook capture)", 0,
			"an onHook capture stamps a datagram when the process READS it, so reads bunch "
			"through the joiner's blocking loads: wire timing (scoreboard_cadence) reads the "
			"run's external.pcapng" },
	{ "packet.sid_seq_ack", MaskKind::Field, '*', 0, "sid/seq/ack", 0,
			"the protocol SID is drawn per connection and seq/ack count from it: they key "
			"origin packets within a run, never compare across runs" },
	{ "event.gameplay_body", MaskKind::Field, '*', 0, "C 0x0c / S 0x0a body", 0,
			"per-frame positions and ticks: these bodies compare as their decoded state "
			"records" },
	{ "uplink.carrier", MaskKind::Field, 'C', 0x00C, "carrier", 0,
			"the ground entity handle is session-local: compared as none/bound, and as the "
			"count of its changes between consecutive uplinks. The word is the body's ground "
			"link +0x28 (NetPacket_SerializePlayerState @0x4C09C0: the read @0x4C142B, the "
			"handle word @0x4C1950) and the position goes out relative to its pose "
			"(Entity_TransformWorldToLocal @0x4C1961, the store @0x4C197C; the host's apply "
			"back to world @0x4C1DE1), so each change is a change of the position's frame of "
			"reference: x7's fragrespawn RR, RR-aa and OR change it once (none to the hut "
			"0x2011 at the respawn), where a joiner whose link flips each record sends "
			"positions in two frames" },
	{ "uplink.priority_handle", MaskKind::Field, 'C', 0x00C, "priority_handle", 0,
			"priority handles are session-local: compared as occupied/empty" },
	{ "uplink.priority_score", MaskKind::Field, 'C', 0x00C, "priority_score", 0,
			"a score is a per-frame distance/view weight to moving entities "
			"(Server_BuildEntityPriorityListForPlayer's top-4 interest pairs): compared as "
			"per-slot distributions, the occupancy exact" },
	{ "frame.carried", MaskKind::Field, 'S', 0x00A, "carried", 0,
			"the carried object handle is session-local: compared as none/bound" },
	{ "frame.mount_handle", MaskKind::Field, 'S', 0x00A, "mount_handle", 0,
			"the mount handle is session-local: compared as none/bound" },
	{ "frame.timer_seconds", MaskKind::Field, 'S', 0x00A, "timer_seconds", 0,
			"the round timer counts with the session clock: the timer block compares its "
			"state bytes" },
	{ "frame.server_stats", MaskKind::Field, 'S', 0x00A, "timer_state[2..3]", 0,
			"the server-status block's fps and CPU bytes are the host's measured main-loop "
			"frame rate and CPU share (g_StatsAvgFps / g_StatsCpuPercent, copied every "
			"Server_TickUpdate @0x51D7E0..0x51D7FB): machine-load measurements; "
			"frame_server_cpu_fill keeps a never-written CPU byte visible" },
	{ "frame.phase_counter", MaskKind::Field, 'S', 0x00A, "flags2", 0,
			"flags2 is the per-slot send counter (++ before every 0x0A, 1 at the first: "
			"Server_SendEntityStateToPlayer @0x517BE8): which 8-bit values fall in the "
			"steady window depends on how many frames preceded it, so the route nibble and "
			"the modal step compare; the route nibbles a window holds are a phase slice until "
			"it spans one full cycle, so their set compares only with 16 steady frames or "
			"more on both sides (x5's kick, five or six frames: RR {6..10} against RR-aa "
			"{8..13}, 0.625 apart)" },
	{ "frame.mount_phase", MaskKind::Field, 'S', 0x00A, "mount record", 0,
			"the mounted-ammo record rides only a 0x0A whose flags2 low nibble is 8 "
			"(NetPacket_WritePlayerState @0x4FF6B0, the test @0x4FFD94..0x4FFD9A), one frame "
			"in 16: a window shorter than the cycle holds it or not by the counter's phase "
			"(x5's kick, five or six frames before the punt: RR {0, none} against RO's {0}), so "
			"the mount set compares over a full cycle on both sides, as frame_flags2 does. The "
			"record reads the body's mount word +0x16C (@0x4FFDA4, 0xFFFF when null @0x4FFEC7), "
			"the word the recipient's own record names as its carrier with its attach bone, "
			"else the ground entity at bone 0 (NetPacket_SerializePlayerState @0x4C09C0: "
			"@0x4C0A00..0x4C0A08, @0x4C0A1A), so each record is keyed by the own record's "
			"carrier state in its frame, and a state compares only when both runs carried a "
			"record in it or both stayed in it a full cycle: a shorter stay shows its record "
			"when a nibble-8 frame falls inside it. x6's drive turret stay is 10 frames on RR, "
			"RR-aa and OR: RR (flags2 0x4F..0x58) and RR-aa (0x50..0x59) caught its 0x58 "
			"frame, OR (0x4D..0x56) did not; x3 and x4's RR and RR-aa missed it where their "
			"RO or OR caught it" },
	{ "frame.self_offset_rests", MaskKind::Field, 'S', 0x00A, "own record offset", 0,
			"the recipient's own record encodes -CameraOffset (Server_SendEntityStateToPlayer "
			"@0x517BA0: the anchor @0x517BEF..0x517C13); between two rests the eye moves along "
			"one curve a tick at a time (a death's fall, a hit's tilt decaying) that the "
			"recipient's frames sample at its send cadence (the per-slot counter @0x517BE8), so "
			"which points of a curve a window holds is the frame phase: x6's death curves are "
			"one curve on RR, RR-aa, RO and OO at phases 8, 10, 11 and 0 ticks (L3-s6), and RR "
			"against RR-aa stood 0.14 to 0.57 apart on the sampled points (selfnade 0.14, "
			"permdeath and fragrespawn 0.44, hostkill 0.57). A rest (one offset in two or more "
			"consecutive records) compares as its 1/16 key and the records between two rests "
			"as one transition 'from>to'; a carrier-local record cuts the sequence, and a "
			"transition a cut or the window's edge leaves open is not compared. A tilt between "
			"two rests is its own transition; one that runs straight into a death is inside "
			"the death's. x6's 01tr-coop-aikill corpse rests at z -3.21 sixteenths on RR and "
			"-2.10 on RR-aa, the death's own draw, which the A/A carries" },
	{ "frame.health_hit", MaskKind::Field, 'S', 0x00A, "health residual", 0,
			"a hit's damage is the round's impact speed times its weight / 875 "
			"(Weapon_CalcImpactDamage @0x4EC920, @0x4ECB2B) times its zone's multiplier: on a "
			"type-3 target the switch @0x4EC9BF gives 1.25 for zones 0..4 (flt_7C6F18), 1.0 for "
			"5..8 and, by its default leg (@0x4EC9B6), for 19 and above (the fld1 @0x4EC94D "
			"kept), 0.5 for 9..12 and 15..18 (flt_7C3B94, @0x4EC9F1) and 3.0 for 13..14 "
			"(flt_7C6F80, @0x4EC9CD) with the target's +0x2C flag 0x800 (@0x4EC9C6); a type-3 "
			"target whose def carries flag 0x200 (@0x4EC968) takes the seat path instead, 6.0 "
			"for the hit's seat types 2, 3, 6 and 7 (flt_7CD510, @0x4EC977..0x4EC98E) with the "
			"same flag (@0x4EC994), else 1.0, as every other target. On a type-3 target the "
			"shooter's byte (+0x15E58 at the round's +0x26C, @0x4ECA1E..0x4ECA24) then scales "
			"the damage by 0.9 for 1 (flt_7C459C, @0x4ECB66) or 1.1 for 2 (flt_7CD50C, "
			"@0x4ECB5A), and the ammo def's minimum and positive maximum clamp it "
			"(@0x4ECB77..0x4ECB91), so a residual between a run's full health and death is "
			"where the round landed and how fast it arrived: "
			"x5's 01tr-coop-aikill RR read {0, 150, 36} against RR-aa's {0, 150, 8} (base 114 "
			"at 1.0 and at 1.25), x1's RR-aa 7 (base 115 at 1.25). The full and dead values "
			"compare exactly and a residual as 'hit'" },
	{ "frame.weapon_countdown", MaskKind::Field, 'S', 0x00A, "weapon[1..4]", 0,
			"the weapon block rides one frame in four (NetPacket_WritePlayerState @0x4FF6B0, "
			"the phase test @0x4FF81B) and its slot bytes +0x168, +0x170, +0x16C and +0x164 "
			"(weapon[1..4], @0x4FF845, @0x4FF876, @0x4FF8A1, @0x4FF8BB; the first two only "
			"while the body's +0x24 mask 0x02 is set, @0x4FF843 / @0x4FF874) are counters the "
			"host steps down once a second (Server_TickUpdate @0x51D7E0, the per-slot pass "
			"@0x51DFBA..0x51E011), so a countdown's first sample is its first second or the "
			"next, by its start against the block's phase, and a countdown the window cuts ends "
			"where the window does. Each countdown compares over the values both runs cover: a "
			"top one second higher (or one running when the window opened) and a bottom below "
			"the other's window-cut end leave the set, and a sample that leaves it takes its "
			"frame's whole weapon block with it, so the other weapon fields of those one or "
			"two edge frames go uncompared. x6 retail: hostkill RR first sampled "
			"(5, 120) and RR-aa (4, 119), their window-cut ends 79 and 78; selfnade RR's +0x168 "
			"countdown first read 5, RR-aa's 4; swap RR 59 against RR-aa 60" },
	{ "frame.env_tod", MaskKind::Field, 'S', 0x00A, "tod_fixed", 0,
			"the time of day advances with the session clock: the env block compares the "
			"weather fields" },
	{ "entity.handle", MaskKind::Field, 'S', 0x00A, "handle", 0,
			"entity handles are allocated per session: motion groups per handle, never by "
			"number" },
	{ "state.position", MaskKind::Field, '*', 0, "position/orientation/analog", 0,
			"motion follows independent input: origin-normalized range and step within 8x "
			"of RR, and a 64-unit initial baseline. Entity positions compare decompressed "
			"and anchor-added in world 16.16 (a compact code is sign/exponent/mantissa "
			"relative to the recipient's eye anchor, Network_CompressFixedPoint @0x4C2780, "
			"not a linear coordinate), carrier-local records skipped; frame_self_offset "
			"keeps the anchor's own offset visible" },
	{ "state.transient_lead", MaskKind::Field, '*', 0, "dynamic sequence start", 0,
			"a dynamic sequence (one handle's records, the uplink stream, the frame anchor) "
			"whose first in-window sample differs from the next while the one after repeats "
			"that next one comes to rest on its second sample, and the first is a change the "
			"window's edge cut: it sets no baseline. x7's hostkill RR opened on one record of "
			"the host body mid-pose (x 1036.9, heading 0, pitch 247) and then (64,247) from "
			"0.195 s, where RR-aa and RO opened on (64,247): RR read orientation range 65 "
			"against RR-aa's 2. x7's selfnade: the camp AI 0x0035's lone aim-change record "
			"(210,3), sent once, then (210,210) at every frame from the joiner's death, fell "
			"1.34, 1.53 and 2.38 s into the window on RR, RR-aa and RO (the AI's think "
			"14.8..17.1 s after the join on the three retail hosts, the window opening at the "
			"runner's readiness) and 0.81 s before it on OR. A sequence with no rest at its "
			"second sample keeps its first" },
	{ "integrity.key", MaskKind::Field, '*', 0, "challenge key", 0,
			"each integrity challenge's key is random per request: CRCs compare with the "
			"key XORed out" },
	{ "integrity.oracle", MaskKind::Field, '*', 0, "row CRC oracle", 0,
			"a retail host validates every reply and punts a bad one "
			"(NapiNPServerMsg_HandleAntiCheatCRCCheck @0x502050, @0x5021da): with a retail "
			"host and no PUNT a reply RR never witnessed is witnessed. A row's CRC is a "
			"corpus property, so RR-aa's rows of the same integrity profile join RR's "
			"oracle" },
	{ "integrity.family_toggle", MaskKind::Field, 'S', 0x030, "challenge family", 0,
			"the 0x30 / 0x31 family is one process-global toggle that every scoreboard "
			"boundary flips (Server_BroadcastWeaponSlotStateToPlayers @0x508540: the read "
			"@0x50859E, the flip @0x50868E after the slot loop, its only writer), called each "
			"311 ticks (Server_TickUpdate @0x51D912), while a slot takes its challenge only in "
			"state 6 with a body whose +0x24 bit 1 (mask 0x02) is clear (@0x508584, "
			"@0x508594..0x508598): a "
			"boundary the joiner spends outside that (x6's deaths) flips the family unseen, so "
			"within one mission a challenge's family is its "
			"predecessor's flipped once per boundary between them (x6's fragrespawn: RR, RR-aa "
			"and RO's retail host skip two boundaries over the death and continue the family, "
			"OO skips three and repeats it, both as the toggle runs). The boundary clock "
			"restarts with each mission (Nbstat_StartupInit @0x4FDE41) after a load of its own "
			"length, so the count does not carry across the S2C 0x25 that ends a mission "
			"(Server_DisconnectAndResetAllPlayerSlots @0x516160, the send @0x516385): x6's "
			"round RO retail host began its second mission on the family it ended the first "
			"on, RR and RR-aa on the other" },
	{ "integrity.join_adm", MaskKind::Field, 'S', 0x031, "ammo challenge row", 0,
			"an S2C 0x31 carries the ammo row of the recipient body's equipped ADM, entity+0x2B0 "
			"(Server_BroadcastWeaponSlotStateToPlayers @0x508540: the lookup called @0x508605, "
			"the row "
			"@0x50863E), which the plyr class init writes (PlayerClass_InitEntity @0x4B1116) "
			"until the joiner's first C2S 0x0C writes its own (@0x4C20A3) or a round it fires "
			"does (Server_ClientFiredRound @0x50BD56): between a C2S 0x0B and the first C2S "
			"0x0C after it every host challenges the class-init ADM's row, and whether a "
			"boundary falls in that span is the join's phase against the 311-tick clock. x6's "
			"ash-i5a-aas-deploy RO retail host sent row 14 0.53 s after the join, before the "
			"joiner's first 0x0C; its RR and RR-aa never met the span. Such a challenge leaves "
			"the row sets, the CRC oracle and the unwitnessed rows; its reply's shape still "
			"counts (integrity_pairs)" },
	{ "integrity.held_adm", MaskKind::Field, 'S', 0x031, "ammo challenge row", 0,
			"an S2C 0x31 carries the round row (AdmDef+4) of the ADM the body holds at the "
			"boundary (Server_BroadcastWeaponSlotStateToPlayers @0x508540: +0x2B0 @0x5085F6, "
			"the row @0x50863E), the ADM the joiner's C2S 0x0C writes (@0x4C20A3), and the "
			"0x30 / 0x31 family flips at every boundary (@0x50859E, @0x50868E), so an ammo "
			"challenge falls every second boundary: an ADM the uplinks name for less than two "
			"boundaries in either run is challenged or not by its stay's phase. x6: selfnade "
			"and permdeath select the grenade (uplink ADM 52, round row 54) about 0.8 s into "
			"the window and die 6.5 s later; selfnade's RR, RR-aa and RO's retail host met an "
			"ammo boundary in that stay and OR's host did not, permdeath's RR and RR-aa did not "
			"and OR's host did; fragrespawn holds ADM 52 for 89 s and every run challenges row "
			"54. Such a challenge leaves the row sets and the unwitnessed rows; RR's CRC still "
			"joins the oracle and the run's still checks against it" },
	{ "count.reload_echo", MaskKind::Field, '*', 0, "C2S 0x25 / S2C 0x49 count", 0,
			"a slot whose clip is empty runs an auto-reload loop: the idle action finds the "
			"clip empty with rounds in the pool and auto reload on and queues RELOAD "
			"(WeaponAction_Idle @0x542920: the phase write @0x542960, WeaponSlot_RequestReload "
			"called @0x5429AF, which queues action 4 while the phase byte +0x5A's sign bit is "
			"clear, @0x53F11C / @0x53F12F); the transition enters it on phase 1 with the "
			"action's delayStart timer (WeaponAction_ProcessFrame @0x540E60: @0x5413EA / "
			"@0x5413EC); the reload action sends one C2S 0x25 on that phase and sets 0x80 "
			"(WeaponAction_Reload @0x5430B0: the tests @0x5430C0 / @0x5430C6, the send "
			"@0x543103, @0x543108), the action shims then write phase 2 "
			"(ActionSlot_ExecuteActionNoEffect @0x5419F4, ActionSlot_ExecuteActionWithEffect "
			"@0x541881), and the timer's expiry returns it to idle "
			"(ActionSlot_FinishActivePhase called @0x54316E). The host echoes every request to "
			"its sender as S2C 0x49 (NapiNPServerMsg_HandleReloadRequest @0x514DF0, the "
			"sender's copy @0x514EE3), and the loop ends when the first echo refills the clip "
			"(NapiNPClientMsg_WeaponReload_0x049 @0x42C0A0 calling WeaponSlot_ReloadAmmo "
			"@0x541720 @0x42C116: the pool @0x541842, the clip @0x541850). So the count is the "
			"loop's entries before the first echo lands, the echo's latency against the loop's "
			"period: the self-grenade cases' RR and RR-aa runs of x1..x7 carry 4 to 6 requests,"
			" and x1's OR retail joiner (our host echoing) sent one, its echo landing 23 ms "
			"after it on the wire. The pair's counts compare as a lone request against a looped"
			" one, except a lone request whose first echo landed inside the longest entry "
			"spacing the reference's own requests allow (a request datagram that follows "
			"another with no C datagram between carries the n requests entered between the two "
			"sends: n - 1 spacings fit in their gap; x7's RR 2 then 3, 187 ms apart, at most 93"
			" ms), on the external capture's wire time" },
};

inline constexpr size_t kNormalizationCount = sizeof(kNormalizations) / sizeof(kNormalizations[0]);

constexpr bool text_present(const char *text) {
	return text != nullptr && text[0] != '\0';
}

constexpr bool normalizations_have_reasons() {
	for (const Normalization &entry : kNormalizations)
		if (!text_present(entry.id) || !text_present(entry.name) || !text_present(entry.reason))
			return false;
	return true;
}
static_assert(normalizations_have_reasons(), "every masked field cites its nondeterminism source");

constexpr bool has_normalization(std::string_view id) {
	for (const Normalization &entry : kNormalizations)
		if (id == entry.id)
			return true;
	return false;
}

constexpr const Normalization *find_normalization(
		MaskKind kind, char dir, uint16_t selector, std::string_view name = {}) {
	for (const Normalization &entry : kNormalizations) {
		if (entry.kind != kind || entry.selector != selector)
			continue;
		if (entry.dir != '*' && entry.dir != dir)
			continue;
		if (!name.empty() && name != entry.name)
			continue;
		return &entry;
	}
	return nullptr;
}

constexpr char handshake_dir(uint8_t opcode) {
	return (opcode & 0x80) ? 'S' : 'C';
}

// The handshake field `name` of `opcode` is dynamic (its value masked).
constexpr bool handshake_field_masked(uint8_t opcode, std::string_view name) {
	return find_normalization(MaskKind::HandshakeField, handshake_dir(opcode), opcode, name) !=
			nullptr;
}

constexpr bool handshake_cu_masked(uint8_t opcode, std::string_view cu_name) {
	return find_normalization(MaskKind::HandshakeCu, handshake_dir(opcode), opcode, cu_name) !=
			nullptr;
}

constexpr bool handshake_cu_low16(uint8_t opcode, std::string_view cu_name) {
	return find_normalization(
				   MaskKind::HandshakeCuLow16, handshake_dir(opcode), opcode, cu_name) != nullptr;
}

// The size class base of a length-reduced handshake field, 0 when its size is exact.
// The masked handshake field `name` of `opcode` has no fixed size either.
constexpr bool handshake_field_size_masked(uint8_t opcode, std::string_view name) {
	const Normalization *entry =
			find_normalization(MaskKind::HandshakeField, handshake_dir(opcode), opcode, name);
	return entry != nullptr && entry->bytes == 1;
}

constexpr uint32_t handshake_length_class(uint8_t opcode, std::string_view name) {
	const Normalization *entry =
			find_normalization(MaskKind::HandshakeLength, handshake_dir(opcode), opcode, name);
	return entry ? entry->bytes : 0;
}

constexpr bool initial_body_exempt(char dir, uint16_t identity) {
	return find_normalization(MaskKind::InitialBody, dir, identity) != nullptr;
}

constexpr bool initial_body_set(char dir, uint16_t identity) {
	return find_normalization(MaskKind::InitialSet, dir, identity) != nullptr;
}

constexpr bool initial_records(char dir, uint16_t identity) {
	return find_normalization(MaskKind::InitialRecords, dir, identity) != nullptr;
}

constexpr bool length_set_exempt(char dir, uint16_t identity) {
	return find_normalization(MaskKind::LengthExempt, dir, identity) != nullptr;
}

constexpr uint32_t stable_body_prefix(char dir, uint16_t identity) {
	const Normalization *entry = find_normalization(MaskKind::BodyPrefix, dir, identity);
	return entry ? entry->bytes : 0;
}

constexpr bool first_order_excluded(char dir, uint16_t identity) {
	return find_normalization(MaskKind::FirstOrder, dir, identity) != nullptr;
}

constexpr bool group_length_masked(char dir, uint16_t identity) {
	return find_normalization(MaskKind::GroupLength, dir, identity) != nullptr;
}

constexpr bool group_reply(char dir, uint16_t identity) {
	return find_normalization(MaskKind::GroupReply, dir, identity) != nullptr;
}

constexpr bool group_boundary_tolerant(char dir) {
	return find_normalization(MaskKind::GroupBoundary, dir, 0) != nullptr;
}

static_assert(handshake_field_masked(0x42, "SCRK") && !handshake_field_masked(0x42, "PN"));
// EIP/EPN are deterministic (the enumerate target and the host port): unmasked.
static_assert(!handshake_field_masked(0x41, "EIP") && !handshake_field_masked(0x81, "EPN"));
static_assert(handshake_cu_masked(0x82, "NWUID") && !handshake_cu_masked(0x42, "NWUID"));
static_assert(handshake_cu_low16(0x42, "CI0") && !handshake_cu_masked(0x42, "CI0"));
static_assert(
		handshake_length_class(0x82, "SCRK") == 62 && handshake_length_class(0x82, "SK") == 0);
static_assert(initial_body_exempt('S', 0x019) && !initial_body_exempt('C', 0x019));
static_assert(initial_body_exempt('S', 0x01A) && !initial_body_exempt('S', 0x064));
static_assert(initial_body_set('S', 0x016) && !initial_body_exempt('S', 0x016));
static_assert(initial_records('S', 0x010) && length_set_exempt('S', 0x010));
static_assert(initial_records('S', 0x00D) && length_set_exempt('S', 0x00D));
static_assert(stable_body_prefix('S', 0x00F) == 4 && stable_body_prefix('C', 0x00F) == 0);
static_assert(first_order_excluded('S', 0x030) && !first_order_excluded('S', 0x02C));
static_assert(group_length_masked('S', 0x00A) && !group_length_masked('C', 0x00A));
static_assert(group_reply('S', 0x057) && !group_reply('C', 0x057));
static_assert(group_boundary_tolerant('C') && !group_boundary_tolerant('S'));

const char *mask_kind_name(MaskKind kind);

} // namespace opennova::parity
