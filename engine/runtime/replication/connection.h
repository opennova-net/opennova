#pragma once

#include <array>
#include <cstdint>

#include <runtime/world/entity.h> // EntityHandle

#include <runtime/inmatch/session_transport.h> // ISessionTransport

namespace opennova::replication {

// The witnessed transport mode a connection carries [orig: CGameSession_SetConnectionMode
// @0x4c49f0 -> g_NapiNPCtx.transport_mode +0x50; CNapiNetwork_SetTransportMode @0x4c8750
// opens a socket only for 2/3/4 — mode 1 is in-process]. A connection's mode IS the
// original's mode, not an invented type (ADR 0011).
enum class TransportMode : uint8_t {
	Loopback = 1,   // socketless: the host's own local client (LoopbackChannel)
	Client = 2,     // a remote LAN/MP peer joining a host (socket)
	HostClient = 3, // host + client (the listen server itself opens a socket)
};

// One client connection on the authoritative host — the reimpl of a NapiNPConnection node on
// the server's connection list [orig: the list NapiNPServer_SendFiltered @0x4C87E0 walks, one
// SendToConn @0x4c4f20 per node]. The transport is NON-OWNING: the Godot binding (or a test
// harness) owns the LoopbackChannel / UdpSessionTransport; the per-connection drain/fan
// primitives (connection_fan.h) only fan the per-frame world reference over it and drain its
// C2S queue. The connection TABLE is owned by the host driver — inmatch's Server_TickUpdate
// over NapiNPProtocol.connection_list (each node embeds a Connection `link`).
struct Connection {
	// The byte transport for this peer (host's own client = a LoopbackChannel; a remote peer =
	// a UdpSessionTransport). Never null for a registered connection.
	ISessionTransport *transport = nullptr;

	// The witnessed transport mode (above). The host's own client is Loopback.
	TransportMode mode = TransportMode::Loopback;

	// The pool-0 player entity this connection drives — the SUBJECT its S2C 0x0A frame is
	// anchored to, and the owner the host verifies a C2S 0x0C uplink against [orig:
	// NetPacket_DispatchEntityPacketCallback @0x4D6A80 `entity == *owner_ctx`]. The host spawns it for
	// a joiner (spawn_remote_player, bound by the host driver). An INVALID handle = pre-spawn
	// (still handshaking), which cannot enter the per-player 0x0A writer. The host's own
	// loopback binds its local player through the same host spawn path.
	// [orig: Server_SendEntityStateToPlayer @0x517BA0 state==6 gate]
	world::EntityHandle owned_entity{};

	// Per-connection visibility/filter descriptor [orig: g_NapiNPCtx send_mask +0x1198].
	// PRESENT but UNREAD this increment — the 2-peer co-op MVP broadcasts the whole world to
	// every connection (faithful to a filter==1 send [orig: @0x510ED0]). The per-connection
	// SendFiltered cull is a deferred optimization; the field is here so it slots in without an
	// API change.
	uint32_t send_mask = 0;

	// Allocation lifetime of `owned_entity`. Packed retail handles are reused, so
	// the handle alone can silently retarget a connection after a despawn/slot
	// reuse. Zero means "not stamped yet" (the pre-World reply path binds only a
	// wire handle); the first live authoritative use stamps the registry serial.
	// The original player slot owns a live entity pointer and the per-frame writer
	// runs only for its deployed state, so a freed entity is never replaced by an
	// unrelated same-slot allocation behind the writer's back.
	// [orig: Server_SendEntityStateToPlayer @0x517BA0 state==6 gate and recipient
	// entity eye read @0x517BF5..0x517C13]
	uint64_t owned_entity_spawn_id = 0;

	// Per-connection S2C 0x0A sub-block phase — the reimpl of the original's per-player-slot send
	// counter [orig: playerSlot+100566, ++ before every 0x0A in Server_SendEntityStateToPlayer
	// @0x517be8]. NetPacket_WritePlayerState @0x4ff6b0 writes it as the frame's `flags2` byte and
	// `phase & 3` selects the header sub-block (0 weapon / 1 server-status / 2 env / 3 gametype),
	// while `phase & 0xF == 8` gates the mounted-weapon ammo block. emit_connection_s2c advances it.
	uint8_t s2c_phase = 0;
	uint32_t hud_hit_feedback_serial = 0;
    // One-frame NAK backoff; silence keeps reducing each frame until receive.
    // [orig: sub_4C62A0 @0x4c62a0; Server_SendEntityStateToPlayer @0x517c58]
    bool nak_backoff_pending = false;
    uint32_t receive_silence_ms = 0;

	// Per-connection entity AGE bytes for the 0x0A priority loop — the reimpl of the original's
	// per-player-slot age arrays: pool-0 ages at slot+89978 (256 B), pool-1 at slot+90234 (+256)
	// [orig: Server_BuildEntityPriorityList @ 0x50e590 saturating `paddusb +1` sweep @0x50e60f].
	// Index = (pool==1 ? 256 : 0) + (slot & 0xFF). Aged +1 (saturating) every emit, reset to 0
	// when the entity's record is serialized [orig: @0x50f168]; age raises the priority key and
	// age >= 50 force-admits past the 1124-tile distance gate, so budget-starved entities climb
	// until they win a slot — the original's round-robin is EMERGENT from aging (no resume cursor).
	std::array<uint8_t, 512> s2c_entity_age{};

	// Per-slot last-SENT caches feeding the priority delta terms (same 512-slot
	// indexing as the ages): heading = the entity's rounded yaw high byte
	// `(Yaw + 0x800000) >> 24`, speed = the tick-displacement metric `|v| >> 6`
	// clamp 255. Both written when the entity's record is serialized, beside the
	// age reset; the build side scores `2*|headingDelta| + 3*|speedDelta|` so
	// entities whose motion CHANGED since their last record jump the queue.
	// [orig: slot+91434 / slot+92890; writes @0x50f17c / the @0x50f22x tail;
	// reads @0x50e905 / @0x50ea77]
	std::array<uint8_t, 512> s2c_entity_heading{};
	std::array<uint8_t, 512> s2c_entity_speed{};

	// The client's 4 requested-interest pairs from its C2S 0x0C extended uplink
	// (handle u16, score u16 zero-extended). The 0x0A priority build walks them:
	// a row named by a live slot takes that score as a FLOOR on its computed
	// priority and passes the 1124-tile distance gate regardless of range — the
	// client's way of holding its ridden vehicle / watched target in its stream.
	// [orig: the 0x0C read-apply stores at playerState+94346 (handles) /
	//  +94356 (scores) @0x4c09c0; the walk + floor in
	//  Server_BuildEntityPriorityList @0x50e590]
	std::array<uint16_t, 4> tracked_handle{{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};
	std::array<int32_t, 4> tracked_score{{0, 0, 0, 0}};

	// Per-connection round-event watermark: the newest world.out.rounds sequence already swept
	// into this connection's 0x0A tag-2 stream [orig: playerSlot+97544, stamped = stat_id after
	// each Server_BuildRoundEventListForPlayer @0x4ffee0 sweep; its non-zero gate skips the walk
	// until the player is armed]. Armed on the first in-match emit at the CURRENT ring sequence,
	// so a joiner never receives the pre-join round backlog. (D-NET-152)
	uint32_t round_watermark = 0;
	bool round_watermark_armed = false;

	// RESPAWN-PENDING / undeployed — the reimpl of the player slot's stateByte bit4
	// (slot+89912 & 0x10). Set at join iff the mission offers deploy-selectable spawn zones,
	// UNCONDITIONAL on the spectator latch — a join-time spectator holds the bit forever
	// [orig: Server_OnPlayerJoin @0x51a6f2 `|= 0x10 iff SpawnZoneList_GetCount() > 0`];
	// cleared only by the deploy leg [orig: Server_ProcessPlayerDeath @0x517791
	// `and 0xEF`]. While set: the connection's 0x0A header flags1 carries bit1 EVERY frame
	// (the client's deploy screen is held open by it — one flags1 bit1=0 frame closes it
	// [orig: NetPacket_WritePlayerState @0x4ff7bd; client g_DeployScreenActive = (flags1 & 2) != 0
	// @0x42ff82]; a spectator client's free-fly ignores the held bit), the pre-deploy
	// player entity carries the hidden bit0, the 0x0E handler accepts a deploy from an
	// alive-but-undeployed player (the dead-or-pending gate @0x519cc7 — retail places no
	// spectator check at that layer either), and the priority build's flat branch reads
	// this bit for its dead-or-spectator recipient predicate [orig: @0x50e68c]. The t35
	// deploy-idle punt explicitly EXEMPTS spectators [orig: @0x51e11f]. Death does NOT
	// set it — the death screen is client-local (D-NET-156).
	// The live player-slot spectator mode. Admission and F3 both land here;
	// 0x75, 0x16, and 0x0A flags1 bit0 read this one canonical bit
	// [orig: playerSlot+100567; NetPacket_WritePlayerState @0x4ff795]. The
	// slot hide byte 97536 tracks this latch exactly in JO (writers PlayerAdd
	// @0x51d0ce / OnPlayerJoin @0x51a79c / the permadeath conversion @0x519e76;
	// 97537 and the paired gate 96481 are read-but-never-set), so the
	// owner-hidden replication stamp reduces to this bit (D-NET-217).
	bool spectator = false;
	bool respawn_pending = false;

	// Retail's post-death player-slot counters. Both are whole seconds and are
	// decremented by the authority's 1 Hz player maintenance. +360 rejects every
	// C2S 0x0E deployment pick; +364 rejects only picks that resolve to a real
	// spawn target. The phase-0 S2C 0x0A header exposes their low bytes, with
	// +360 suppressed unless the owned entity is dead.
	// [orig: GameEvent_PlayerDeath @0x516ec4..0x516eeb;
	// Server_ProcessClientRequestRespawn @0x519c67/@0x519cf2;
	// NetPacket_WritePlayerState @0x4ff81b]
	uint32_t respawn_delay_seconds = 0;      // playerSlot+360
	uint32_t spawn_target_hold_seconds = 0; // playerSlot+364
	bool respawn_hold_armed = false;

	// The armory-reuse cooldown (whole seconds) and the pre-round loadout latch.
	// A nonzero-class C2S 0x2F is accepted only while the cooldown has expired
	// or the pre-round timer runs; an accept re-arms it from the host's
	// `armory_reuse_time` unless the latch is set, and clears the latch. Join and
	// every deploy zero the cooldown and set the latch iff pre-round; the 1 Hz
	// player maintenance decrements it.
	// [orig: playerSlot+356 (slot[89]) — seed @0x515ba6, gate @0x5158d0, zero
	//  @0x51a752/@0x517900, decrement @0x51e00b..0x51e022; playerSlot+89912 bit 1 —
	//  set @0x51a6e2/@0x517812, test @0x515b96, clear @0x515bb3]
	int32_t armory_reuse_seconds = 0;
	bool preround_loadout_latch = false;

	// The emote cooldown (whole seconds): a C2S 0x14 emote fans only while it
	// is zero, and every fan re-arms it to 2; the 1 Hz player maintenance
	// counts it down (a negative value clamps to zero).
	// [orig: playerSlot+376 — the gate and the store NapiNPServerMsg_HandleEmoteRequest
	//  @0x501e55 / @0x501f53; the decrement Server_TickUpdate @0x51e028..0x51e03f]
	int32_t emote_cooldown_seconds = 0;
	// The radio-call cooldown (whole seconds), the same shape: a C2S 0x13 call
	// is handled only while it is zero and re-arms it to 4.
	// [orig: playerSlot+380 — the gate NapiNPServerMsg_HandleRadioCall @0x514386,
	//  the stores @0x5144d0 / @0x514716; the decrement Server_TickUpdate
	//  @0x51e045..0x51e05c]
	int32_t radio_call_cooldown_seconds = 0;

	// Consecutive periodic seconds this player has carried a 4091/4093/4095 flag
	// in CTF / FlagBall / Flag Me; at the host's `flag_reset_seconds` the carry is
	// broken and the carrier killed. [orig: playerSlot+89872 (slot[22468]) in
	//  Server_CheckPlayerViolations @0x51ac5a..0x51ac77]
	uint32_t flag_carry_seconds = 0;

	// Retail's downed/medic player-slot state. +368 is the whole-second revive
	// window (armed to 120 for a revivable player death and decremented at 1 Hz).
	// +372 is the inverse OPTIONS_AUTOMEDIC preference: zero on the retail wire
	// means automatic requests are enabled. The separate request latch supplies
	// bit seven of both S2C 0x54 and player-sync field 0x0008.
	// [orig: GameEvent_PlayerDeath @0x516DD0; NapiNPServerMsg_AutoMedicPreference
	// @0x501BE0; NetPacket_SerializePlayerSync0x46 @0x505E80]
	uint32_t downed_revive_seconds = 0; // playerSlot+368
	// The team-mode 1 Hz 0x46 field-0x0008 resend walks every dead slot whose
	// entity+44 cause bits 0x400 (knife) / 0x800 (headshot) are clear. Our Entity
	// carries no cause word: the cause rides RoundDeath::event_flags, so the
	// death transaction latches the predicate here and the deploy clears it.
	// [orig: Weapon_CalcImpactDamage @0x4EC920 writes entity+44 |= 0x800
	//  @0x4ec9c6/@0x4ec994; Server_TickUpdate reads `(entity+44 & 0xC00) == 0`
	//  @0x51e333]
	bool death_cause_revivable = false;
	bool auto_medic_enabled = true;     // inverse playerSlot+372
	// playerSlot+89856. Set by the C2S 0x2E medic-request handler (the
	// Server_BroadcastMedicRequest @0x515390 port in
	// server_message_dispatch.cpp), cleared by the deploy and the death
	// transaction; read by the 0x54 / 0x46-0x0008 bit-7 encoders.
	bool medic_request_active = false;

	// Number of 32-host-tick samples for which the player's eye
	// (Position.Z + CameraOffset.Z) has remained strictly below the water
	// plane. The first sample past 4 * breathtime (the WAC named value; sample
	// 81 at its default 20) kills the player; GameEvent_PlayerDeath reads the
	// still-live value to select drowned event 26. A surfaced or dead-flagged
	// sample clears it.
	// [orig: playerSlot+460 in Server_UpdatePlayerBreathTimers @0x50D770;
	// GameEvent_PlayerDeath @0x5172EC..0x51732A]
	uint32_t underwater_breath_samples = 0;

	// The authority tick of the last completed deployment. Validity is explicit
	// because tick zero is a real stamp and unsigned subtraction preserves the
	// original counter's wrap behavior. A death within 620 ticks forces the
	// +360/+364 hold to three seconds even when the configured timeout is larger.
	// [orig: playerSlot+96456 read @0x516edc; deployment stamp in the
	// Server_ProcessPlayerDeath path @0x517740]
	uint32_t last_deploy_tick = 0;
	bool last_deploy_tick_valid = false;
};

} // namespace opennova::replication
