#pragma once

// The normalization registry (ADR 0050 R4): every field a comparison masks,
// strips or reduces, each with the nondeterminism source that makes masking it
// correct. The comparator's masks are driven FROM this table (the handshake
// dynamic fields, the initial-body exemptions, the stable-body prefixes, the
// first-order exclusions), and every other masking site asserts its entry at
// compile time (static_assert(has_normalization("...")) beside the code), so a
// mask cannot exist without a registered reason. opennova-parity also writes the
// table into every verdict.

#include <cstdint>
#include <string_view>

namespace opennova::parity {

enum class MaskKind : uint8_t {
	HandshakeField, // a 0x41/0x42/0x81/0x82 flat-TLV field's value: `opcode`, `name`
	HandshakeCu, // a CU chunk's value inside a handshake: `opcode`, the CU `name`
	InitialBody, // an initial-stream body left out of byte equality: `dir`, `identity`
	BodyPrefix, // the first `bytes` of a stable body: `dir`, `identity`
	FirstOrder, // an identity left out of the first-occurrence order: `dir`, `identity`
	Field, // a decoded field the metrics reduce (named by `id` at the use site)
};

struct Normalization {
	const char *id;
	MaskKind kind;
	char dir; // 'C', 'S', or '*' (either direction / not directional)
	uint16_t selector; // the handshake opcode, or the 9-bit message identity
	const char *name; // the TLV / CU field name, or the decoded field
	uint32_t bytes; // BodyPrefix: the stripped prefix length
	const char *reason;
};

// A message identity: the dispatch tag plus the settings-table bit.
constexpr uint16_t identity_of(uint8_t tag, bool settings) {
	return static_cast<uint16_t>(tag | (settings ? 0x100u : 0u));
}

inline constexpr Normalization kNormalizations[] = {
	// --- the outer handshake (the retired verifier's dynamic field lists)
	{ "handshake.0x41.CI", MaskKind::HandshakeField, 'C', 0x41, "CI", 0,
			"the client's connection index, assigned per process" },
	{ "handshake.0x41.EIP", MaskKind::HandshakeField, 'C', 0x41, "EIP", 0,
			"the client's external IP: the machine's adapter address" },
	{ "handshake.0x41.EPN", MaskKind::HandshakeField, 'C', 0x41, "EPN", 0,
			"the client's external port: an ephemeral UDP port" },
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
			"the client's session crypt key, drawn per session" },
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
	{ "handshake.0x81.EIP", MaskKind::HandshakeField, 'S', 0x81, "EIP", 0,
			"the echo of the client's external IP" },
	{ "handshake.0x81.EPN", MaskKind::HandshakeField, 'S', 0x81, "EPN", 0,
			"the echo of the client's ephemeral external port" },
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
			"the server's session crypt key, drawn per session" },
	{ "handshake.0x82.RIP", MaskKind::HandshakeField, 'S', 0x82, "RIP", 0,
			"the client's reflected IP (adapter address)" },
	{ "handshake.0x82.RPN", MaskKind::HandshakeField, 'S', 0x82, "RPN", 0,
			"the client's reflected port: an ephemeral UDP port" },
	{ "handshake.0x42.CU.UdpCode1", MaskKind::HandshakeCu, 'C', 0x42, "UdpCode1", 0,
			"a per-session UDP reachability code the client draws" },
	{ "handshake.0x42.CU.UdpCode2", MaskKind::HandshakeCu, 'C', 0x42, "UdpCode2", 0,
			"a per-session UDP reachability code the client draws" },
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
	{ "initial.S.0x46", MaskKind::InitialBody, 'S', 0x046, "PLAYER_SYNC", 0,
			"a flag-gated player-slot delta whose score/timer fields vary per process" },
	{ "initial.S.0x68", MaskKind::InitialBody, 'S', 0x068, "LOADED_MODEL_PAGE_REQUEST", 0,
			"the page cursor advances on a process-global wrap" },

	// --- stable bodies
	{ "stable.S.0x0f.tick", MaskKind::BodyPrefix, 'S', 0x00F, "WORLD_STATE_LOAD", 4,
			"S2C 0x0F opens with the live session tick; the rest is the same-case pose and "
			"flags" },

	// --- first-occurrence order
	{ "order.S.0x30", MaskKind::FirstOrder, 'S', 0x030, "ENTITY_CHECKSUM_REQ", 0,
			"0x30/0x31 alternate on one process-global phase, so a join can enter either half" },
	{ "order.S.0x31", MaskKind::FirstOrder, 'S', 0x031, "LOADOUT_CRC_REQ", 0,
			"0x30/0x31 alternate on one process-global phase, so a join can enter either half" },

	// --- decoded fields the metrics reduce
	{ "trace.session", MaskKind::Field, '*', 0, "session", 0,
			"the client UDP port is ephemeral: each run binds its one gameplay session" },
	{ "trace.ts_ns", MaskKind::Field, '*', 0, "ts_ns", 0,
			"capture timestamps are wall clock: only rates, gaps and windows compare" },
	{ "packet.sid_seq_ack", MaskKind::Field, '*', 0, "sid/seq/ack", 0,
			"the protocol SID is drawn per connection and seq/ack count from it: they key "
			"origin packets within a run, never compare across runs" },
	{ "event.gameplay_body", MaskKind::Field, '*', 0, "C 0x0c / S 0x0a body", 0,
			"per-frame positions and ticks: these bodies compare as their decoded state "
			"records" },
	{ "uplink.carrier", MaskKind::Field, 'C', 0x00C, "carrier", 0,
			"the ground entity handle is session-local: compared as none/bound" },
	{ "uplink.priority_handle", MaskKind::Field, 'C', 0x00C, "priority_handle", 0,
			"priority handles are session-local: compared as occupied/empty, scores exact" },
	{ "frame.carried", MaskKind::Field, 'S', 0x00A, "carried", 0,
			"the carried object handle is session-local: compared as none/bound" },
	{ "frame.mount_handle", MaskKind::Field, 'S', 0x00A, "mount_handle", 0,
			"the mount handle is session-local: compared as none/bound" },
	{ "frame.timer_seconds", MaskKind::Field, 'S', 0x00A, "timer_seconds", 0,
			"the round timer counts with the session clock: the timer block compares its "
			"state bytes" },
	{ "frame.env_tod", MaskKind::Field, 'S', 0x00A, "tod_fixed", 0,
			"the time of day advances with the session clock: the env block compares the "
			"weather fields" },
	{ "entity.handle", MaskKind::Field, 'S', 0x00A, "handle", 0,
			"entity handles are allocated per session: motion groups per handle, never by "
			"number" },
	{ "state.position", MaskKind::Field, '*', 0, "position/orientation/analog", 0,
			"motion follows independent input: origin-normalized range and step within 8x "
			"of RR, and a 64-unit initial baseline" },
	{ "integrity.key", MaskKind::Field, '*', 0, "challenge key", 0,
			"each integrity challenge's key is random per request: CRCs compare with the "
			"key XORed out" },
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

// The handshake field `name` of `opcode` is dynamic (its value masked).
constexpr bool handshake_field_masked(uint8_t opcode, std::string_view name) {
	const char dir = (opcode & 0x80) ? 'S' : 'C';
	return find_normalization(MaskKind::HandshakeField, dir, opcode, name) != nullptr;
}

constexpr bool handshake_cu_masked(uint8_t opcode, std::string_view cu_name) {
	const char dir = (opcode & 0x80) ? 'S' : 'C';
	return find_normalization(MaskKind::HandshakeCu, dir, opcode, cu_name) != nullptr;
}

constexpr bool initial_body_exempt(char dir, uint16_t identity) {
	return find_normalization(MaskKind::InitialBody, dir, identity) != nullptr;
}

constexpr uint32_t stable_body_prefix(char dir, uint16_t identity) {
	const Normalization *entry = find_normalization(MaskKind::BodyPrefix, dir, identity);
	return entry ? entry->bytes : 0;
}

constexpr bool first_order_excluded(char dir, uint16_t identity) {
	return find_normalization(MaskKind::FirstOrder, dir, identity) != nullptr;
}

static_assert(handshake_field_masked(0x42, "SCRK") && !handshake_field_masked(0x42, "PN"));
static_assert(handshake_cu_masked(0x82, "NWUID") && !handshake_cu_masked(0x42, "NWUID"));
static_assert(initial_body_exempt('S', 0x019) && !initial_body_exempt('C', 0x019));
static_assert(stable_body_prefix('S', 0x00F) == 4 && stable_body_prefix('C', 0x00F) == 0);
static_assert(first_order_excluded('S', 0x030) && !first_order_excluded('S', 0x016));

const char *mask_kind_name(MaskKind kind);

} // namespace opennova::parity
