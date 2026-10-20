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
	uint32_t bytes; // BodyPrefix: the stripped prefix length; HandshakeLength: the class base
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
	{ "order.C.0x20", MaskKind::FirstOrder, 'C', 0x020, "ENTITY_CHECKSUM_REPLY", 0,
			"answers S2C 0x30, whose half of the 0x30/0x31 alternation is process-global" },
	{ "order.C.0x21", MaskKind::FirstOrder, 'C', 0x021, "CHECKSUM_REPLY", 0,
			"answers S2C 0x31, whose half of the 0x30/0x31 alternation is process-global" },

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
	{ "group.S.0x57.reply", MaskKind::GroupReply, 'S', 0x057, "RTT_ECHO", 0,
			"the host answers each client ping (queued every client frame, "
			"Client_ProcessNetworkFrame @0x42c41c) from its receive pump, and the reply rides "
			"the next boundary's packet: how many ride with the gameplay record follows the "
			"client's frame rate against the host's boundary. A steady gameplay packet keys "
			"without them; a packet of replies alone keeps them" },
	{ "group.steady_order", MaskKind::Field, '*', 0, "record order within a packet", 0,
			"a reply the receive pump queues (S2C 0x57 for a C2S 0x2C, C2S 0x2C for a S2C "
			"0x57) lands before or after the tick's own records by when its request arrived: "
			"the connection queue (CNapiNPConnection_QueueMessage @0x628640) is packed in "
			"queue order at the boundary (BuildOutgoingPackets @0x628430). A steady packet "
			"groups as its record multiset; steady_order_distribution judges the order" },
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
			"the ground entity handle is session-local: compared as none/bound" },
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
			"the modal step compare" },
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
	{ "integrity.key", MaskKind::Field, '*', 0, "challenge key", 0,
			"each integrity challenge's key is random per request: CRCs compare with the "
			"key XORed out" },
	{ "integrity.oracle", MaskKind::Field, '*', 0, "row CRC oracle", 0,
			"a retail host validates every reply and punts a bad one "
			"(NapiNPServerMsg_HandleAntiCheatCRCCheck @0x502050, @0x5021da): with a retail "
			"host and no PUNT a reply RR never witnessed is witnessed. A row's CRC is a "
			"corpus property, so RR-aa's rows of the same integrity profile join RR's "
			"oracle" },
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
