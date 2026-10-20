#include <runtime/world/weapon_fire_gate.h>
// D-NET-152 — the C2S 0x06 client-fired-round pipeline. The dispatch case must validate the
// shooter (anti-spoof vs the connection's own entity [orig: @0x51358d]), refuse a combo the
// player holds no weapon in, enforce the held slot's clip for
// armory-known primary fire ("NO AMMO!" reject [orig: @0x50c15c]; decrement [orig:
// Weapon_ConsumeAmmo @0x540913]), mirror the fired weapon ahead of every refusal after the
// dead-on-a-carrier return [orig: @0x50bd56] + the claimed
// target [orig: @0x50c2ad], and append ONE g_RoundRing event carrying the client's exact
// pre-spread fire pose [orig: RoundData_AddRound @0x4fdb40] — with NO reactive reply (the echo
// rides the per-frame 0x0A tag-2 fan, §5.9.1). The 0x25 relay refills the same slot's clip
// [orig: WeaponSlot_ReloadAmmo @0x541720 @0x514F03].
//
// Coverage: a valid primary fire (ring fields + clip 30->29 + mirrors + no replies/sends); a
// spoofed shooter handle and an unknown adm are dropped; alt fire appends without touching the
// clip; the host's own loopback 0x06 is a net-path no-op [orig: @0x50c18d]; clip exhaustion
// rejects further fire until the 0x25 relay refills; a no-clip weapon (clipsize -1) never
// rejects; a table-less host accepts without bookkeeping.

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_tick.h>

#include <runtime/replication/connection.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>

#include <runtime/world/ai.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_weapon.h> // controller_fire_pose
#include <runtime/world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "conn_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

// The player entity's own weapon table (world::Entity::weapon_slots), the host's
// per-player slot rows since D-NET-336's move.
std::map<uint16_t, w::PersonalWeaponSlot> &slots_of(w::World &world,
		const inmatch::NapiNPConnection &conn) {
	return world.registry.get(conn.link.owned_entity)->weapon_slots;
}
using conn_fixture::make_seeded_conn;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

w::PlayerSpawn player_spawn(uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = {0, 0, 0};
	s.net_id = net_id;
	return s;
}

void put_u16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(uint8_t(v & 0xFF));
	b.push_back(uint8_t(v >> 8));
}
void put_u32(std::vector<uint8_t> &b, uint32_t v) {
	b.push_back(uint8_t(v & 0xFF));
	b.push_back(uint8_t((v >> 8) & 0xFF));
	b.push_back(uint8_t((v >> 16) & 0xFF));
	b.push_back(uint8_t((v >> 24) & 0xFF));
}

// Craft the fixed 45-B C2S 0x06 body (§5.16 field order).
std::vector<uint8_t> fire_body(uint16_t shooter, uint8_t fire_flags, uint8_t adm,
                               int32_t px, int32_t py, int32_t pz, int32_t dx, int32_t dy,
                               uint16_t target, uint16_t hit_part, uint8_t extra2,
                               uint8_t misc) {
	std::vector<uint8_t> b;
	static uint32_t next_client_tick = 0x01000000u;
	put_u32(b, next_client_tick++); // beyond any 0x61 seed; each new shot advances time
	put_u16(b, shooter);
	b.push_back(fire_flags);
	b.push_back(adm);
	put_u32(b, uint32_t(px));
	put_u32(b, uint32_t(py));
	put_u32(b, uint32_t(pz));
	put_u32(b, uint32_t(dx));
	put_u32(b, uint32_t(dy));
	put_u16(b, target);
	put_u16(b, hit_part);
	b.push_back(0x07);            // extra_byte1
	b.push_back(extra2);          // extra_byte2 -> ring subtype composite
	b.push_back(misc);            // misc_byte -> ring slot_byte
	put_u16(b, 0);                // delta_x
	put_u16(b, 0);                // delta_y
	put_u16(b, 0);                // delta_z
	put_u16(b, 0);                // delta_yaw
	put_u16(b, 0);                // delta_pitch
	return b;
}

int dispatch_fire(inmatch::NapiNPConnection &conn, std::vector<inmatch::NapiNPConnection> &roster,
                  w::World &world, const std::vector<uint8_t> &body) {
	std::vector<ProtocolMessage> msgs;
	msgs.push_back(make_protocol_message(0x06, body));
	std::vector<ProtocolMessage> replies =
			inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn, msgs, 100, roster, &world);
	return int(replies.size());
}

void dispatch_gameplay(uint8_t tag, const std::vector<uint8_t> &body,
		inmatch::NapiNPConnection &conn,
		std::vector<inmatch::NapiNPConnection> &roster, w::World &world) {
	std::vector<ProtocolMessage> msgs;
	msgs.push_back(make_protocol_message(tag, body));
	(void)inmatch::dispatch_session_replies(
			inmatch::GameConfig{}, conn, msgs, 100, roster, &world);
}

bool check_mounted_slot_select_fire_and_reload() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	w::AiSystem &ai = world.ai;
	const w::EntityHandle shooter =
			w::spawn_remote_player(world, player_spawn(0xFFF1));
	if (!expect(shooter.valid(), "mounted-route shooter spawned")) return false;

	world.tables.weapons.entries.resize(7);
	w::WeaponTableEntry &child_weapon = world.tables.weapons.entries[5];
	child_weapon.name = "WPN_CHILD";
	child_weapon.category = 3;
	child_weapon.rank = 2;
	child_weapon.clipsize = 4;
	child_weapon.startrounds = 12; // an 8-round reserve
	child_weapon.ammo_class_count = 1;
	child_weapon.valid = true;
	w::WeaponTableEntry &parent_weapon = world.tables.weapons.entries[6];
	parent_weapon.name = "WPN_PARENT";
	parent_weapon.category = 4;
	parent_weapon.rank = 3;
	parent_weapon.clipsize = 9;
	parent_weapon.startrounds = 30; // a 21-round reserve
	parent_weapon.ammo_class_count = 1;
	parent_weapon.valid = true;

	w::Entity parent;
	parent.kind = w::EntityKind::Item;
	parent.has_item_def = true;
	parent.item_type = 1;
	parent.item_attrib = w::kItemAttribEweap;
	parent.primary_weapon = parent_weapon.name;
	const w::EntityHandle parent_h = world.registry.spawn(1, parent);
	w::Entity *live_parent = world.registry.get(parent_h);

	w::Entity child;
	child.kind = w::EntityKind::Item;
	child.has_item_def = true;
	child.item_type = 2;
	child.item_attrib = w::kItemAttribEweap;
	child.emplacement_attachment_flags = 0x02;
	child.emplacement_parent = parent_h;
	child.emplacement_parent_spawn_id = live_parent->registry_spawn_id;
	child.ground_target = parent_h;
	child.primary_weapon = child_weapon.name;
	const w::EntityHandle child_h = world.registry.spawn(1, child);
	w::Entity *live_child = world.registry.get(child_h);
	w::Entity *player = world.registry.get(shooter);
	player->mounted = true;
	player->mount_target = child_h;
	player->mount_type = w::SeatType::Gunner;
	player->use_gun_slot_swapped = true;
	player->equipped_adm_index = 5;

	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(
			2, 1, nullptr, ns::TransportMode::Client, shooter, true));

	MountedWeaponSlotSelection select_parent;
	select_parent.use_parent_slot = true;
	dispatch_gameplay(c2s::MOUNTED_WEAPON_SLOT_SELECT,
			encode_mounted_weapon_slot_selection(select_parent),
			roster[0], roster, world);
	if (!expect(live_child->primary_weapon_slot.redirect_to_parent_slot &&
			player->equipped_adm_index == 6,
			"C2S 0x16 nonzero selects the validated groundEntity vehicle slot"))
		return false;

	// The authoritative C2S 0x06 must spend the selected world MountSlot, not
	// the connection's personal combo map. The request ADM follows EquippedSlot.
	dispatch_fire(roster[0], roster, world,
			fire_body(shooter.packed, 0x22, 6, 0, 0, 0, 0, 0,
					0xFFFF, 1, 0, 0));
	if (!expect(live_parent->primary_weapon_slot.clip == 8 &&
			live_child->primary_weapon_slot.clip == 4 &&
			slots_of(world, roster[0]).empty(),
			"mounted parent-route fire spends only the parent world slot"))
		return false;
	// A request naming the child's weapon on the parent route fires the parent's: the
	// carrier's slot fires under the def it holds, whatever the request names
	// [orig: Server_ClientFiredRound @0x50C1E3..0x50C30D].
	dispatch_fire(roster[0], roster, world,
			fire_body(shooter.packed, 0x22, 5, 0, 0, 0, 0, 0,
					0xFFFF, 3, 0, 0));
	if (!expect(world.out.rounds.count == 2 && world.out.rounds.records[1].adm_index == 6 &&
			live_parent->primary_weapon_slot.clip == 7 &&
			live_child->primary_weapon_slot.clip == 4,
			"a mounted fire fires the carrier slot's weapon whatever the request names"))
		return false;

	WeaponReload reload;
	reload.entity_handle = child_h.packed;
	reload.reload_param = 0xBEEF; // ignored for EWeap entities in retail
	dispatch_gameplay(c2s::WEAPON_RELOAD_REQUEST, encode_weapon_reload(reload),
			roster[0], roster, world);
	// On the parent route the reload splits across the two entities: the parent's
	// slot gives the loaded 7, the draw's size (9) and takes the clip; the child's
	// own slot takes the refund (7 + 8 + 4, capped at 12: 4 / 8) and pays the draw
	// (min(9, 8) = 8: its reserve 0). The parent reads 8 / 21, the child 4 / 0.
	// [orig: WeaponSlot_ReloadAmmo @0x541730 -> Entity_GetWeaponSlots
	//  @0x54610E..0x546113; WeaponSlot_AddAmmo @0x540AB0 -> @0x5462A0 on the
	//  child; Entity_GetMountSlotReserve @0x54078E; Entity_SpendMountSlotReserve @0x5409C3; the
	//  clip store @0x541850]
	if (!expect(live_parent->primary_weapon_slot.clip == 8 &&
					live_parent->primary_weapon_slot.reserve == 21 &&
					live_child->primary_weapon_slot.clip == 4 &&
					live_child->primary_weapon_slot.reserve == 0,
			"a parent-route reload draws on the child's own reserve into the parent's clip"))
		return false;

	MountedWeaponSlotSelection select_child;
	dispatch_gameplay(c2s::MOUNTED_WEAPON_SLOT_SELECT,
			encode_mounted_weapon_slot_selection(select_child),
			roster[0], roster, world);
	if (!expect(!live_child->primary_weapon_slot.redirect_to_parent_slot &&
			player->equipped_adm_index == 5,
			"C2S 0x16 zero selects the child's embedded MountSlot"))
		return false;
	dispatch_fire(roster[0], roster, world,
			fire_body(shooter.packed, 0x22, 5, 0, 0, 0, 0, 0,
					0xFFFF, 2, 0, 0));
	if (!expect(live_child->primary_weapon_slot.clip == 3 &&
			live_parent->primary_weapon_slot.clip == 8,
			"mounted child-route fire spends only the child world slot"))
		return false;
	dispatch_gameplay(c2s::WEAPON_RELOAD_REQUEST, encode_weapon_reload(reload),
			roster[0], roster, world);
	// 3 + 0 + 3 capped at 12: 4 / 2, then the draw of min(4, 2): 2 / 0.
	if (!expect(live_child->primary_weapon_slot.clip == 2 &&
					live_child->primary_weapon_slot.reserve == 0,
			"mounted reload refills the selected child world slot from its reserve"))
		return false;

	// Malformed bodies and failed parent validation are consume-and-ignore: no
	// route or EquippedSlot mutation.
	dispatch_gameplay(c2s::MOUNTED_WEAPON_SLOT_SELECT, {1},
			roster[0], roster, world);
	if (!expect(!live_child->primary_weapon_slot.redirect_to_parent_slot &&
			player->equipped_adm_index == 5,
			"short C2S 0x16 body cannot mutate route state"))
		return false;
	live_child->ground_target = w::EntityHandle{};
	dispatch_gameplay(c2s::MOUNTED_WEAPON_SLOT_SELECT,
			encode_mounted_weapon_slot_selection(select_parent),
			roster[0], roster, world);
	return expect(!live_child->primary_weapon_slot.redirect_to_parent_slot &&
			player->equipped_adm_index == 5,
			"invalid groundEntity parent cannot select or partially mutate the route");
}

// Exercise the actual 0x43 session receive boundary, not the message dispatcher in isolation.
// Retail's HandleSessionPacket drops seq <= recv_ack_seq before ParseMessages, so replaying the
// identical UDP datagram must neither append another round nor spend another cartridge.
bool check_duplicate_c2s_session_does_not_refire() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::AiSystem &ai = world.ai;
	const w::EntityHandle shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	if (!expect(shooter.valid(), "session replay shooter spawned")) return false;

	world.tables.weapons.entries.resize(6);
	w::WeaponTableEntry &rifle = world.tables.weapons.entries[5];
	rifle.name = "WPN_TESTRIFLE";
	rifle.category = 3;
	rifle.rank = 2;
	rifle.clipsize = 30;
	rifle.ammo_index = 1;
	rifle.valid = true;
	world.tables.ammo.entries.resize(2);
	world.tables.ammo.entries[1].name = "REMOTE_POWER_THROW";
	world.tables.ammo.entries[1].velocity = 620;
	world.tables.ammo.entries[1].max_age_ticks = 248;
	world.tables.ammo.entries[1].valid = true;

	const PeerAddr peer{0x0100007Fu, 30123};
	const std::string client_scrk = "CLIENT-REPLAY-SCRK";
	const std::string server_scrk = "SERVER-REPLAY-SCRK";
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	inmatch::NapiNPConnection conn =
			make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, shooter, true);
	conn.peer = peer;
	conn.client_scrk = client_scrk;
	conn.server_scrk = server_scrk;
	conn.client_ck = 0x11223344u;
	conn.server_sk = 0x55667788u;
	ctx.np_protocol.connection_list.push_back(std::move(conn));
	// The slot the loadout grant left.
	slots_of(world, ctx.np_protocol.connection_list[0])[uint16_t(3 * 65 + 2)] =
			w::PersonalWeaponSlot{5, 30};

	const std::vector<uint8_t> shot =
			fire_body(shooter.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 1, 12, 0);
	SessionSequencing client_tx{1, 0};
	std::vector<uint8_t> session_body;
	if (!expect(frame_session_packet(client_tx, SessionCrypto{client_scrk, {}, 0x55667788u},
	                                 {make_protocol_message(0x06, shot)}, session_body),
	            "frame C2S 0x06 session packet"))
		return false;
	const std::vector<uint8_t> fire_datagram =
			nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(session_body));

	inmatch::handle_server_datagram(ctx, peer, fire_datagram.data(), fire_datagram.size(), 100);
	if (!expect(world.out.rounds.count == 1, "first C2S 0x06 appends one authoritative round"))
		return false;
	const uint16_t combo = uint16_t(3 * 65 + 2);
	if (!expect(slots_of(world, ctx.np_protocol.connection_list[0])[combo].clip == 29,
	            "first C2S 0x06 spends one cartridge"))
		return false;

	inmatch::handle_server_datagram(ctx, peer, fire_datagram.data(), fire_datagram.size(), 101);
	if (!expect(world.out.rounds.count == 1, "exact duplicate C2S 0x06 does not append a second round"))
		return false;
	if (!expect(slots_of(world, ctx.np_protocol.connection_list[0])[combo].clip == 29,
	            "exact duplicate C2S 0x06 does not spend a second cartridge"))
		return false;

	std::vector<uint8_t> heartbeat_body;
	if (!expect(frame_session_packet(client_tx, SessionCrypto{client_scrk, {}, 0x55667788u},
	                                 {make_protocol_message(0x34, {})}, heartbeat_body),
	            "frame next contiguous C2S session packet"))
		return false;
	const std::vector<uint8_t> heartbeat_datagram =
			nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(heartbeat_body));
	inmatch::handle_server_datagram(ctx, peer, heartbeat_datagram.data(), heartbeat_datagram.size(), 102);
	inmatch::handle_server_datagram(ctx, peer, fire_datagram.data(), fire_datagram.size(), 103);
	if (!expect(ctx.np_protocol.connection_list[0].seq.last_inbound_seq == 2,
	            "replayed older C2S packet cannot regress the host ACK latch"))
		return false;

	std::vector<uint8_t> ack_datagram;
	if (!expect(inmatch::frame_in_match_s2c(ctx, peer, 0x34, {}, ack_datagram),
	            "host frames an ACK-bearing S2C packet"))
		return false;
	uint8_t opcode = 0;
	std::vector<uint8_t> ack_body;
	ProtocolPacketHeader ack_hdr;
	std::vector<ProtocolMessage> ack_messages;
	if (!expect(nw_decode_inbound(ack_datagram.data(), ack_datagram.size(), opcode, ack_body) &&
	                    opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
	                    decode_protocol_packet_plaintext(ack_body.data(), ack_body.size(), server_scrk,
	                                                     ack_hdr, ack_messages),
	            "decode host ACK-bearing S2C packet"))
		return false;
	return expect(ack_hdr.ack_count == 2, "host echoes the highest contiguous C2S sequence");
}

bool check_retail_fire_admission() {
    w::World world;
    world.registry.configure_pool(0, 4);
    const auto shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
    std::vector<inmatch::NapiNPConnection> roster;
    roster.push_back(conn_fixture::make_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
    auto &conn = roster[0];
    world.tables.weapons.entries.resize(6);
    auto &weapon = world.tables.weapons.entries[5];
    weapon.name = "FRESHNESS_RIFLE";
    weapon.valid = true;
    weapon.clipsize = 20;
    weapon.category = 3;
    weapon.rank = 2;
    weapon.action_fsm.actions[w::weapon_action::kFire].delay_start = 100; // not in the sum
    weapon.action_fsm.actions[w::weapon_action::kFire].delay_end = 2;
    weapon.action_fsm.actions[w::weapon_action::kRecoil].delay_start = 3;
    weapon.action_fsm.actions[w::weapon_action::kRecoil].delay_end = 4;
    slots_of(world, conn)[uint16_t(3 * 65 + 2)] = w::PersonalWeaponSlot{5, 20}; // the grant's
    const auto send = [&](uint32_t tick, uint8_t flags = 0, uint8_t adm = 5) {
        ClientFiredRound fire;
        fire.current_tick = tick;
        fire.shooter_handle = shooter.packed;
        fire.target_handle = 0xFFFF;
        fire.adm_index = adm;
        fire.fire_flags = flags;
        dispatch_fire(conn, roster, world, encode_client_fired_round(fire));
    };
    send(100);
    if (!expect(world.out.rounds.count == 0, "unseeded host player cannot fire")) return false;
    (void)inmatch::Server_RerollPlayerTickSeed(conn, world.out.rounds.frame_clock);
    const uint32_t seed = conn.tick_seed;
    send(0);
    send(seed);
    if (!expect(world.out.rounds.count == 0, "zero and seed-equal fire ticks are rejected")) return false;
    send(seed + 1);
    if (!expect(world.out.rounds.count == 1, "first tick beyond seed fires")) return false;
    send(seed + 1);
    send(seed + 10);
    if (!expect(world.out.rounds.count == 1, "duplicate and cooldown-equal shots are rejected")) return false;
    send(seed + 11);
    if (!expect(world.out.rounds.count == 2, "fire-end plus recoil delays define cooldown")) return false;
    world.rules.cease_fire = true;
    send(seed + 100);
    if (!expect(world.out.rounds.count == 2, "cease-fire rejects the shot before ammo or stamp changes")) return false;
    world.rules.cease_fire = false;
    conn.link.spectator = true;
    send(seed + 100);
    if (!expect(world.out.rounds.count == 2, "spectator shots are rejected")) return false;
    conn.link.spectator = false;
    send(seed + 100, 0, 4); // invalid weapon must not consume the stamp
    send(seed + 21, 1);    // alt fire advances by zero, not the primary cooldown
    send(seed + 22, 1);
    if (!expect(world.out.rounds.count == 4, "rejects preserve the clock and alt shots stamp only their tick")) return false;
    if (!expect(slots_of(world, conn)[uint16_t(3 * 65 + 2)].clip == 18,
            "only the two accepted primary shots consume cartridges")) return false;
    // The grace reads the server frame clock (g_ServerFrameClock), not the logic
    // tick: the frame clock trails the logic tick by one here, so a logic-tick
    // reader would refuse the second shot. [orig: PlayerSlot_IsActive
    // `cmp g_ServerFrameClock, eax; jl` @0x4FC78E..0x4FC794; the disarm's stamp
    // Server_SendRandomSeedToPlayer @0x51022A / @0x510237]
    (void)inmatch::Server_DisarmPlayerTickSeed(conn, 100);
    world.out.rounds.frame_clock = 100;
    world.logic_tick = 101;
    send(seed + 23, 1);
    world.out.rounds.frame_clock = 102;
    world.logic_tick = 103;
    send(seed + 23, 1); // the grace arm checks host time, not monotonic packet time
    world.out.rounds.frame_clock = 103; // default SP send period 1, strict 3-period boundary
    world.logic_tick = 104;
    send(seed + 23, 1);
    if (!expect(world.out.rounds.count == 6,
            "disarmed player accepts in-flight shots only before the three-period deadline")) return false;
    (void)inmatch::Server_RerollPlayerTickSeed(conn, world.out.rounds.frame_clock);
    send(conn.tick_seed);
    return expect(world.out.rounds.count == 6,
            "re-deployment resets freshness to the newly advertised seed");
}

} // namespace


bool check_fire_owner_environment_and_origin() {
    w::World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(1, 4);
    const auto shooter = w::spawn_remote_player(world, player_spawn(1));
    auto *player = world.registry.get(shooter);
    world.tables.weapons.entries.resize(2);
    auto &weapon = world.tables.weapons.entries[1];
    weapon.valid = true;
    weapon.clipsize = 4;
    weapon.category = 1;
    weapon.ammo_index = 1;
    weapon.ammo_class_id = 1;
    ns::UdpSessionTransport udp(ns::UdpSessionTransport::Role::Host);
    std::vector<inmatch::NapiNPConnection> roster;
    roster.push_back(make_seeded_conn(3, 1, &udp, ns::TransportMode::Client, shooter, true));
    slots_of(world, roster[0])[uint16_t(65)] = w::PersonalWeaponSlot{1, 4}; // the grant's
    auto shoot_at = [&](int32_t px) {
        dispatch_fire(roster[0], roster, world,
                fire_body(shooter.packed, 2, 1, px, 0, 0, 0, 0, 0xFFFF, 1, 0, 0));
    };
    auto shoot = [&] { shoot_at(0); };
    player->flags |= 2;
    shoot();
    if (!expect(world.out.rounds.count == 0, "dead player cannot fire")) return false;
    player->flags &= ~2u;
    weapon.flags = w::weapon_flag::kEmplaced;
    shoot();
    if (!expect(world.out.rounds.count == 0, "unmounted emplaced fire rejected")) return false;
    weapon.flags = 0;
    world.env.water_z = player->eye_offset_z + 1;
    shoot();
    if (!expect(world.out.rounds.count == 0, "submerged head rejects before spending")) return false;
    world.env.water_z = 0;
    weapon.clipsize = -1;
    weapon.ammo_class_count = 2;
    roster[0].reply.ammo_pools[1] = 1;
    shoot();
    if (!expect(world.out.rounds.count == 0, "no-clip weapon still needs its ammo cost")) return false;
    roster[0].reply.ammo_pools[1] = 2;
    shoot();
    if (!expect(world.out.rounds.count == 1, "the clipless weapon fires once its cost is held")) return false;
    // The fire action's re-entry judges the claimed origin's reach: a round claimed
    // 100 m off the shooter is dropped [orig: Server_ClientFiredRound's local pass
    //  @0x50C33C..0x50C6FA, the re-entry @0x42BF34].
    shoot_at(100 * 65536);
    if (!expect(world.out.rounds.count == 1, "a claimed origin beyond the reach fires no round")) return false;
    // The server's real CanFire path follows groundEntity into a carried
    // vehicle and its friendly FARP; it does not reject arbitrary busy children.
    w::Entity farp;
    farp.has_item_def = true;
    farp.item_attrib2 = 0x2000;
    farp.team = 1;
    const auto farp_handle = world.registry.spawn(1, farp);
    w::Entity carrier;
    carrier.has_item_def = true;
    carrier.item_type = 1;
    carrier.carry_flags = 0x40;
    carrier.team = 1;
    carrier.ground_target = farp_handle;
    const auto carrier_handle = world.registry.spawn(1, carrier);
    player->ground_target = carrier_handle;
    shoot();
    if (!expect(world.out.rounds.count == 1, "friendly FARP rejects the C2S fire before spending")) return false;
    player->ground_target = {};
    weapon.ammo_index = 0;
    shoot();
    if (!expect(world.out.rounds.count == 1, "retail zero-index ammo requires Medic attribute")) return false;
    world.tables.class_attribute_flags[(player->player_class - 1) & 15] = 8;
    shoot();
    if (!expect(world.out.rounds.count == 2, "Medic passes the zero-index ammo gate")) return false;

    const w::FixedVec3 inside{2 * 65536, 0, 2 * 65536};
    const w::FixedVec3 outside{2 * 65536 + 1, 0, 0};
    if (!expect(w::weapon_fire_origin_status(world, *player, weapon, inside, false) == 0,
                "on-foot horizontal and vertical limits are independent and inclusive")) return false;
    if (!expect(w::weapon_fire_origin_status(world, *player, weapon, outside, false) == -17,
                "direct fire rejects one Q16 unit beyond two meters")) return false;
    weapon.flags = 0x200;
    if (!expect(w::weapon_fire_origin_status(world, *player, weapon, outside, false) == 0,
                "authored range exemption skips direct-fire distance validation")) return false;
    weapon.flags = 0;
    w::Entity mount;
    mount.bound_radius = 4.0f;
    const auto mounted = world.registry.spawn(1, mount);
    player->mount_target = mounted;
    player->mount_type = w::SeatType::Gunner;
    const w::FixedVec3 diagonal{6 * 65536, 0, 6 * 65536};
    if (!expect(w::weapon_fire_origin_status(world, *player, weapon, diagonal, false) == -17,
                "gunner validates three-dimensional distance against twice the bound")) return false;
    return true;
}

// A refused primary fire still mirrors the fired weapon onto the entity: retail stamps
// entity+688 ahead of its owner checks and the can-fire's NO AMMO, and only the
// dead-on-a-carrier return precedes the stamp.
// [orig: Server_ClientFiredRound - the -10 @0x50BAE8..0x50BB06, the stamp
//  @0x50BD2D..0x50BD56, the -14 @0x50C0A8..0x50C0B9, the -15 @0x50C13A..0x50C169]
bool check_refused_fire_mirrors_the_weapon() {
    w::World world;
    world.registry.configure_pool(0, 4);
    const auto shooter = w::spawn_remote_player(world, player_spawn(1));
    auto *player = world.registry.get(shooter);
    world.tables.weapons.entries.resize(3);
    for (const uint8_t adm : {uint8_t{1}, uint8_t{2}}) {
        auto &weapon = world.tables.weapons.entries[adm];
        weapon.valid = true;
        weapon.clipsize = 4;
        weapon.category = 1;
        weapon.rank = adm;
        weapon.ammo_index = 1;
        weapon.ammo_class_id = 1;
    }
    world.tables.weapons.entries[2].ammo_index = 0; // the zero-index round: Medic only
    ns::UdpSessionTransport udp(ns::UdpSessionTransport::Role::Host);
    std::vector<inmatch::NapiNPConnection> roster;
    roster.push_back(make_seeded_conn(3, 1, &udp, ns::TransportMode::Client, shooter, true));
    // adm 1 holds its slot with an empty clip.
    slots_of(world, roster[0])[uint16_t(65 + 1)] = w::PersonalWeaponSlot{1, 0};
    auto shoot = [&](uint8_t adm) {
        dispatch_fire(roster[0], roster, world,
                fire_body(shooter.packed, 2, adm, 0, 0, 0, 0, 0, 0xFFFF, 1, 0, 0));
    };
    player->equipped_adm_index = 0;
    shoot(1);
    if (!expect(world.out.rounds.count == 0 && player->equipped_adm_index == 1,
                "an empty clip's NO AMMO refusal still mirrors the fired weapon"))
        return false;
    shoot(2);
    if (!expect(world.out.rounds.count == 0 && player->equipped_adm_index == 2,
                "the zero-index round's refusal still mirrors the fired weapon"))
        return false;
    player->flags |= 0x102u;
    shoot(1);
    return expect(world.out.rounds.count == 0 && player->equipped_adm_index == 2,
                  "a dead shooter on a carrier returns ahead of the mirror");
}

// A C2S 0x06 for a combo the player holds no weapon in fires nothing and makes no
// slot: off a mount the round fires through the player's own slot of the fired combo,
// and a slot without a def is refused by retail's can-fire ("NO AMMO!", -15) before
// any clip leg, a clipless weapon's too. A granted slot fires from the clip the grant
// left, never reseeded (B-15).
// [orig: Server_ClientFiredRound @0x50C0C2..0x50C116 -> WeaponSlot_CanFire @0x541BA0
//  (the def test @0x541BB3); the refusal @0x50C13A..0x50C169]
bool check_ungranted_combo_is_refused() {
	w::World world;
	world.registry.configure_pool(0, 4);
	const auto shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
	auto &conn = roster[0];
	world.tables.weapons.entries.resize(7);
	auto &weapon = world.tables.weapons.entries[5];
	weapon.name = "UNGRANTED_RIFLE";
	weapon.valid = true;
	weapon.clipsize = 20;
	weapon.category = 3;
	weapon.rank = 2;
	auto &knife = world.tables.weapons.entries[6];
	knife.name = "UNGRANTED_KNIFE";
	knife.valid = true;
	knife.clipsize = -1;
	knife.category = 1;
	knife.rank = 0;
	const uint16_t combo = uint16_t(3 * 65 + 2);
	const auto shoot = [&](uint8_t adm) {
		dispatch_fire(conn, roster, world,
				fire_body(shooter.packed, 0x22, adm, 0, 0, 0, 0, 0, 0xFFFF, 1, 12, 0));
	};
	shoot(5);
	shoot(6);
	if (!expect(world.out.rounds.count == 0, "an ungranted combo fires no round, clipless too"))
		return false;
	if (!expect(slots_of(world, conn).empty(), "an ungranted combo's refusal makes no slot"))
		return false;
	// The grant left three rounds in the clip: the fire spends one of them.
	slots_of(world, conn)[combo] = w::PersonalWeaponSlot{5, 3};
	shoot(5);
	if (!expect(world.out.rounds.count == 1, "a granted combo fires")) return false;
	return expect(slots_of(world, conn)[combo].clip == 2,
	              "the fire spends the granted clip and never reseeds it");
}

// A slot holding another weapon of the fired combo fires that weapon: the can-fire
// tests the slot under its own def, and the slot's def runs the fire action, so the
// round, its ring adm and its clip are the slot weapon's.
// [orig: Server_ClientFiredRound - the personal slot @0x50C27F, its def @0x50C297, the
//  fire action @0x50C300..0x50C30D; WeaponSlot_CanFire @0x541BA0 reads slot+0x20]
bool check_the_slot_weapon_fires() {
	w::World world;
	world.registry.configure_pool(0, 4);
	const auto shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
	auto &conn = roster[0];
	world.tables.weapons.entries.resize(7);
	for (const uint8_t adm : {uint8_t{5}, uint8_t{6}}) {
		auto &weapon = world.tables.weapons.entries[adm];
		weapon.name = adm == 5 ? "HELD_RIFLE" : "OTHER_RIFLE";
		weapon.valid = true;
		weapon.clipsize = adm == 5 ? 20 : -1;
		weapon.category = 3;
		weapon.rank = 2;
		weapon.ammo_index = adm == 5 ? 1 : 2;
	}
	const uint16_t combo = uint16_t(3 * 65 + 2);
	slots_of(world, conn)[combo] = w::PersonalWeaponSlot{5, 1};
	const auto shoot = [&](uint8_t adm) {
		dispatch_fire(conn, roster, world,
				fire_body(shooter.packed, 0x22, adm, 0, 0, 0, 0, 0, 0xFFFF, 1, 12, 0));
	};
	shoot(6);
	if (!expect(world.out.rounds.count == 1 && world.out.rounds.records[0].adm_index == 5 &&
					slots_of(world, conn)[combo].clip == 0,
			"the other weapon's request fires the held rifle from its clip"))
		return false;
	shoot(6);
	return expect(world.out.rounds.count == 1,
			"the held rifle's empty clip refuses, though the requested weapon has no clip");
}

// A controller seat (under a carrier that is no EWeap item, so the personal route)
// reads every loaded-round bucket as 0 and every pool as 0: a bucket rifle and a
// clipless weapon with a cost refuse, a clip weapon fires from its slot.
// [orig: WeaponSlot_CanFire @0x541C6D..0x541C89 / @0x541CC7..0x541CF6 -> WeaponSlot_GetSharedLoadedRounds
//  @0x540624..0x540627, Entity_GetScoreValueBySlotType @0x54078F..0x540799]
bool check_controller_seat_reads_no_bucket() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	const auto shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
	auto &conn = roster[0];
	world.tables.weapons.ammo_class_names = {"", "CLASS_MANA", "CLS_B"};
	world.tables.weapons.ammo_class_caps = {1000, 1000, 1000};
	world.tables.weapons.entries.resize(8);
	auto weapon = [&](uint8_t adm, int32_t rank, int32_t clipsize, int32_t bucket) {
		auto &e = world.tables.weapons.entries[adm];
		e.name = "SEATED_WEAPON_" + std::to_string(adm);
		e.valid = true;
		e.category = 3;
		e.rank = rank;
		e.clipsize = clipsize;
		e.ammo_class_id = 2;
		e.ammo_class_count = 1;
		e.ammo_bucket = bucket;
		e.ammo_index = 1;
	};
	weapon(5, 2, 20, 1);  // a bucket rifle
	weapon(6, 3, 20, 0);  // a clip rifle
	weapon(7, 4, -1, 0);  // a clipless weapon, one unit a shot
	slots_of(world, conn)[uint16_t(3 * 65 + 2)] = w::PersonalWeaponSlot{5, 0};
	slots_of(world, conn)[uint16_t(3 * 65 + 3)] = w::PersonalWeaponSlot{6, 3};
	slots_of(world, conn)[uint16_t(3 * 65 + 4)] = w::PersonalWeaponSlot{7, 0};
	conn.reply.shared_clips[1] = 5;
	conn.reply.ammo_pools[2] = 50;
	w::Entity carrier;
	carrier.has_item_def = true; // no EWeap bit: the personal route
	w::Entity *player = world.registry.get(shooter);
	player->mounted = true;
	player->mount_type = w::SeatType::Controller;
	player->mount_target = world.registry.spawn(1, carrier);
	const auto shoot = [&](uint8_t adm) {
		dispatch_fire(conn, roster, world,
				fire_body(shooter.packed, 0x22, adm, 0, 0, 0, 0, 0, 0xFFFF, 1, 12, 0));
	};
	shoot(5);
	shoot(7);
	if (!expect(world.out.rounds.count == 0 && conn.reply.shared_clips[1] == 5,
			"a controller seat reads the bucket and the pool as 0: both refuse"))
		return false;
	shoot(6);
	if (!expect(world.out.rounds.count == 1 && slots_of(world, conn)[uint16_t(3 * 65 + 3)].clip == 2,
			"a clip weapon fires from its own slot word"))
		return false;
	player->mounted = false;
	player->mount_type = w::SeatType::None;
	shoot(5);
	return expect(world.out.rounds.count == 2 && conn.reply.shared_clips[1] == 4,
			"off the seat the bucket rifle fires from the player's bucket");
}

// The slot's fire action re-enters with the weapon that fires and judges the round
// again: a slot weapon the owner checks refuse, or a claimed origin beyond the
// shooter's reach, drops the round while its clip is still spent.
// [orig: Server_ClientFiredRound's local pass - the -14 @0x50C0A8..0x50C0B9, the reach
//  @0x50C33C..0x50C6FA; Weapon_ConsumeAmmo @0x542C75]
bool check_the_fire_action_rejudges_the_round() {
	w::World world;
	world.registry.configure_pool(0, 4);
	const auto shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
	auto &conn = roster[0];
	world.tables.weapons.entries.resize(7);
	for (const uint8_t adm : {uint8_t{5}, uint8_t{6}}) {
		auto &weapon = world.tables.weapons.entries[adm];
		weapon.name = adm == 5 ? "MEDIC_ROUND_RIFLE" : "PLAIN_RIFLE";
		weapon.valid = true;
		weapon.clipsize = 20;
		weapon.category = 3;
		weapon.rank = 2;
		weapon.ammo_index = adm == 5 ? 0 : 1; // the zero-index round: Medic only
	}
	const uint16_t combo = uint16_t(3 * 65 + 2);
	slots_of(world, conn)[combo] = w::PersonalWeaponSlot{5, 3};
	w::Entity *player = world.registry.get(shooter);
	const auto shoot = [&](uint8_t adm, int32_t px) {
		dispatch_fire(conn, roster, world,
				fire_body(shooter.packed, 0x22, adm, px, 0, 0, 0, 0, 0xFFFF, 1, 12, 0));
	};
	// The request's weapon passes the first pass; the slot's fails the second.
	shoot(6, 0);
	if (!expect(world.out.rounds.count == 0 && slots_of(world, conn)[combo].clip == 2,
			"the slot weapon's -14 drops the round and still spends the clip"))
		return false;
	world.tables.weapons.entries[5].ammo_index = 1;
	player->flags |= w::kEntityFlagPlayer;
	shoot(6, 3 * 65536); // three metres off the shooter's position
	if (!expect(world.out.rounds.count == 0 && slots_of(world, conn)[combo].clip == 1,
			"a claimed origin beyond the reach drops the round and spends the clip"))
		return false;
	shoot(6, 0);
	return expect(world.out.rounds.count == 1 && world.out.rounds.records[0].adm_index == 5 &&
			slots_of(world, conn)[combo].clip == 0,
			"a round within reach lands");
}

// The ring record of a network primary round carries the fire action's mode byte,
// built from the firing slot's word before the spend: (clip & 3) << 4 | 2; an alt
// round carries the request's flags and its sequence word recomposed with the slot's
// roster index. [orig: WeaponAction_Fire @0x542C11; RoundData_AddRound @0x4FDCDE;
//  Server_ClientFiredRound @0x50BD8F..0x50BDA5]
bool check_the_ring_takes_the_fire_actions_words() {
	w::World world;
	world.registry.configure_pool(0, 4);
	const auto shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
	auto &conn = roster[0];
	conn.reply.player_slot = 3;
	world.tables.weapons.entries.resize(6);
	auto &weapon = world.tables.weapons.entries[5];
	weapon.name = "MODE_RIFLE";
	weapon.valid = true;
	weapon.clipsize = 20;
	weapon.category = 3;
	weapon.rank = 2;
	weapon.ammo_index = 1;
	slots_of(world, conn)[uint16_t(3 * 65 + 2)] = w::PersonalWeaponSlot{5, 7};
	const auto shoot = [&](uint8_t flags, uint16_t hit_part) {
		dispatch_fire(conn, roster, world,
				fire_body(shooter.packed, flags, 5, 0, 0, 0, 0, 0, 0xFFFF, hit_part, 12, 0));
	};
	shoot(0x22, 0x0405);
	shoot(0x23, 0x0C07);
	if (!expect(world.out.rounds.count == 2, "both rounds append")) return false;
	const w::RoundEvent &primary = world.out.rounds.records[0];
	const w::RoundEvent &alt = world.out.rounds.records[1];
	return expect(primary.mode_flags == ((7 & 3) << 4 | 2) && primary.shot_seq == 0x0405 &&
					alt.mode_flags == 0x23 && alt.shot_seq == ((3 << 9) | 0x007),
			"the primary ring takes the pre-spend clip's mode byte, the alt ring its slot's sequence word");
}

// An alt-fire round's byte is an ammo index: the host spawns that ammo, not the
// round of a weapon at that index. [orig: Server_ClientFiredRound @0x50BB0F /
//  @0x50BDF1; RoundData_AddRound hit_params[2] @0x4FDC10]
bool check_an_alt_round_takes_its_ammo_byte() {
	w::World world;
	world.registry.configure_pool(0, 4);
	const auto shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
	auto &conn = roster[0];
	world.tables.weapons.entries.resize(4);
	// A loaded table's slot 0 is the engine's valid "null" def.
	world.tables.weapons.entries[0].name = "null";
	world.tables.weapons.entries[0].valid = true;
	auto &weapon = world.tables.weapons.entries[2];
	weapon.name = "ADM_TWO";
	weapon.valid = true;
	weapon.clipsize = 20;
	weapon.category = 3;
	weapon.rank = 2;
	weapon.ammo_index = 1;
	world.tables.ammo.entries.resize(3);
	world.tables.ammo.entries[1].name = "ADM_TWOS_ROUND";
	world.tables.ammo.entries[1].velocity = 100;
	world.tables.ammo.entries[1].max_age_ticks = 62;
	world.tables.ammo.entries[1].valid = true;
	world.tables.ammo.entries[2].name = "ALT_ROUND";
	world.tables.ammo.entries[2].velocity = 300;
	world.tables.ammo.entries[2].max_age_ticks = 62;
	world.tables.ammo.entries[2].spread_error_fp16 = 30 << 16;
	world.tables.ammo.entries[2].valid = true;
	dispatch_fire(conn, roster, world,
			fire_body(shooter.packed, 0x01, 2, 0, 0, 0, 0, 0, 0xFFFF, 1, 12, 0));
	if (!expect(world.out.rounds.count == 1 && world.round_sim.active_count == 1 &&
					world.round_sim.rounds[0].ammo_index == 2,
			"the alt round flies as ammo 2, its byte"))
		return false;
	// With no weapon the spawn takes the ammo's ERROR leg: a fire straight down +x
	// leaves the axis. [orig: RoundData_SpawnRound @0x4EC38D..0x4EC4D2]
	const w::RandomSpreadOffset spread =
			w::weapon_calc_random_spread_offset(30 << 16, /*seed=*/1, 0, false);
	const w::LiveRound &round = world.round_sim.rounds[0];
	if (!expect((spread.yaw_bam != 0 || spread.pitch_bam != 0) &&
					(round.vel.y != 0.0f || round.vel.z != 0.0f),
			"the weaponless alt round spreads by its ammo's ERROR"))
		return false;
	// The host presents no launch for a remote's alt fire [orig: Server_ClientFiredRound
	//  @0x50BE52..0x50BE64].
	return expect(world.round_sim.fired.empty(),
			"the host draws no launch effect, glow or sound for the alt round");
}

// A 0xFFFF sequence word takes the round pool's fresh id, (the host's slot 0 << 9) | a
// counter, on the spawned round and its ring record; another word stays.
// [orig: CEntityManager_AllocateSlot @0x4EAAD7..0x4EAB04, @0x4EABD5; RoundData_AddRound
//  ring+28 @0x4FDCF1..0x4FDCF5]
bool check_a_ffff_sequence_word_takes_a_fresh_id() {
	w::World world;
	world.registry.configure_pool(0, 4);
	const auto shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
	auto &conn = roster[0];
	world.tables.weapons.entries.resize(6);
	auto &weapon = world.tables.weapons.entries[5];
	weapon.name = "SEQ_RIFLE";
	weapon.valid = true;
	weapon.clipsize = 20;
	weapon.category = 3;
	weapon.rank = 2;
	weapon.ammo_index = 1;
	world.tables.ammo.entries.resize(2);
	world.tables.ammo.entries[1].name = "SEQ_ROUND";
	world.tables.ammo.entries[1].velocity = 100;
	world.tables.ammo.entries[1].max_age_ticks = 62;
	world.tables.ammo.entries[1].valid = true;
	slots_of(world, conn)[uint16_t(3 * 65 + 2)] = w::PersonalWeaponSlot{5, 9};
	const auto shoot = [&](uint16_t hit_part) {
		dispatch_fire(conn, roster, world,
				fire_body(shooter.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, hit_part, 12, 0));
	};
	shoot(0xFFFF);
	shoot(0xFFFF);
	shoot(0x0123);
	if (!expect(world.out.rounds.count == 3 && world.round_sim.active_count == 3,
			"three rounds append and fly"))
		return false;
	const auto &ring = world.out.rounds.records;
	if (!expect(ring[0].shot_seq == 1 && ring[1].shot_seq == 2 && ring[2].shot_seq == 0x0123 &&
					world.round_sim.rounds[0].shot_seq == 1 &&
					world.round_sim.rounds[1].shot_seq == 2,
			"a 0xFFFF word takes the pool's fresh id; another stays"))
		return false;
	// A three-pellet fan allocates ids 3, 4 and 5; its ring takes the last pellet's
	// [orig: Weapon_SpawnProjectileBurstWithSpread @0x4EBC10, the cursor return
	//  @0x4EBE6D].
	world.tables.ammo.entries[1].flags |= w::kAmmoFlagShotgun;
	world.tables.ammo.entries[1].spread_count = 3;
	shoot(0xFFFF);
	return expect(world.out.rounds.count == 4 && ring[3].shot_seq == 5,
			"a fan's ring takes its last pellet's id");
}

// A controller seat under an EWeap carrier fires from the carrier's weapon userpoint for
// the request's barrel: the ring and the round take the posed record when the slot's own
// pre-spend barrel names a point too, else the carrier's raw pose; a request barrel with
// no point keeps the claim. [orig: Server_ClientFiredRound @0x50C1FA..0x50C275;
//  Entity_ComputeUserpointTransform @0x545B46..0x545B4B, @0x545C1B..0x545C41]
struct CarrierPoints final : w::IPoseProvider {
	w::EntityHandle carrier;
	bool resolve_userpoint_frame(w::World &, w::EntityHandle h, const opennova::threedi::Threedi3di3 *,
			int index, int32_t out[6], int32_t *) override {
		if (h != carrier) return false;
		out[0] = index << 12; // a point near the shooter, keyed on the record
		out[1] = 0x1000;
		out[2] = 0;
		out[3] = int32_t(0x1000u << 16);
		out[4] = 0;
		out[5] = 0;
		return true;
	}
};

bool check_a_controller_seat_fires_from_the_userpoint() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	const auto shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	world.tables.weapons.entries.resize(7);
	w::WeaponTableEntry &gun = world.tables.weapons.entries[6];
	gun.name = "WPN_HULL_GUN";
	gun.category = 4;
	gun.rank = 3;
	gun.clipsize = 9;
	gun.startrounds = 9;
	gun.ammo_index = 1;
	gun.valid = true;
	w::Entity hull;
	hull.kind = w::EntityKind::Item;
	hull.has_item_def = true;
	hull.item_type = 1;
	hull.item_attrib = w::kItemAttribEweap;
	hull.primary_weapon = gun.name;
	hull.position = {1.0f, 0.5f, 0.0f};
	hull.weapon_userpoint_bytes[0][0] = 5; // the slot's barrel after one shot (clip 8)
	hull.weapon_userpoint_bytes[2][0] = 4; // the request's barrel
	const w::EntityHandle hull_h = world.registry.spawn(1, hull);
	w::Entity *live_hull = world.registry.get(hull_h);
	if (!expect(world.vehicles.prepare_weapon_slot(*live_hull), "the hull's slot binds"))
		return false;
	CarrierPoints points;
	points.carrier = hull_h;
	world.pose_provider = &points;
	w::Entity *player = world.registry.get(shooter);
	player->mounted = true;
	player->mount_target = hull_h;
	player->mount_type = w::SeatType::Controller;
	player->use_gun_slot_swapped = true;
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(2, 1, nullptr, ns::TransportMode::Client, shooter, true));
	int32_t raw_pose[6] = {};
	(void)w::carrier_weapon_world_pose(world, *live_hull, nullptr, 0, 0, raw_pose);
	// Clip 9: the slot's barrel 1 names no point, so the request's barrel 2 fires from
	// the carrier's raw pose.
	dispatch_fire(roster[0], roster, world,
			fire_body(shooter.packed, 0x22, 6, 0, 0, 0, 0x4000, 0, 0xFFFF, 1, 0, 0));
	// Clip 8: the slot's barrel 0 names one, so barrel 2's record (point 4) is posed.
	dispatch_fire(roster[0], roster, world,
			fire_body(shooter.packed, 0x22, 6, 0, 0, 0, 0x4000, 0, 0xFFFF, 2, 0, 0));
	// Barrel 3 names no point: the claim stands.
	dispatch_fire(roster[0], roster, world,
			fire_body(shooter.packed, 0x32, 6, 0, 0, 0, 0x4000, 0, 0xFFFF, 3, 0, 0));
	world.pose_provider = nullptr;
	if (!expect(world.out.rounds.count == 3, "all three rounds land")) return false;
	const w::RoundEvent &raw = world.out.rounds.records[0];
	const w::RoundEvent &posed = world.out.rounds.records[1];
	const w::RoundEvent &claimed = world.out.rounds.records[2];
	return expect(raw.origin_x == raw_pose[0] && raw.origin_x == 0x10000 &&
					raw.dir_yaw == raw_pose[3] && posed.origin_x == (4 << 12) &&
					posed.origin_y == 0x1000 && claimed.origin_x == 0 &&
					claimed.dir_yaw == int32_t(0x4000u << 16),
			"the slot's barrel gates the posed record; a bare request barrel keeps the claim");
}

int main() {
	if (!check_a_controller_seat_fires_from_the_userpoint()) return 1;
	if (!check_a_ffff_sequence_word_takes_a_fresh_id()) return 1;
	if (!check_an_alt_round_takes_its_ammo_byte()) return 1;
	if (!check_the_ring_takes_the_fire_actions_words()) return 1;
	if (!check_the_fire_action_rejudges_the_round()) return 1;
	if (!check_controller_seat_reads_no_bucket()) return 1;
	if (!check_ungranted_combo_is_refused()) return 1;
	if (!check_the_slot_weapon_fires()) return 1;
	if (!check_refused_fire_mirrors_the_weapon()) return 1;
	if (!check_fire_owner_environment_and_origin()) return 1;
	if (!check_retail_fire_admission()) return 1;
	if (!check_mounted_slot_select_fire_and_reload()) return 1;
	if (!check_duplicate_c2s_session_does_not_refire()) return 1;

	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem &ai = world.ai;
	const w::EntityHandle ha = w::spawn_player(world, player_spawn(0xFFF0));        // host
	const w::EntityHandle hb = w::spawn_remote_player(world, player_spawn(0xFFF1)); // shooter
	const w::EntityHandle hc = w::spawn_remote_player(world, player_spawn(0xFFF2)); // target
	if (!expect(ha.valid() && hb.valid() && hc.valid(), "three players spawned")) return 1;

	// Armory: adm 5 = a 30-round rifle (category 3, rank 2 -> combo 197); adm 7 = a
	// no-clip weapon (clipsize -1, the knife/medpack shape).
	world.tables.weapons.entries.resize(8);
	{
		w::WeaponTableEntry &rifle = world.tables.weapons.entries[5];
		rifle.name = "WPN_TESTRIFLE";
		rifle.category = 3;
		rifle.rank = 2;
		rifle.clipsize = 30;
		rifle.ammo_index = 1;
		rifle.valid = true;
		w::WeaponTableEntry &knife = world.tables.weapons.entries[7];
		knife.name = "WPN_TESTKNIFE";
		knife.category = 1;
		knife.rank = 0;
		knife.clipsize = -1;
		knife.valid = true;
	}
	world.tables.ammo.entries.resize(2);
	world.tables.ammo.entries[1].name = "REMOTE_POWER_THROW";
	world.tables.ammo.entries[1].velocity = 620;
	world.tables.ammo.entries[1].max_age_ticks = 248;
	world.tables.ammo.entries[1].valid = true;

	ns::LoopbackChannel loop;
	ns::UdpSessionTransport udp_b(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport udp_c(ns::UdpSessionTransport::Role::Host);

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(1, 2, &loop, ns::TransportMode::Loopback, ha, true));
	roster.push_back(make_seeded_conn(3, 1, &udp_b, ns::TransportMode::Client, hb, true));
	roster.push_back(make_seeded_conn(4, 1, &udp_c, ns::TransportMode::Client, hc, true));
	// The slots B's loadout grant left: the rifle with a full clip, the knife.
	slots_of(world, roster[1])[uint16_t(3 * 65 + 2)] = w::PersonalWeaponSlot{5, 30};
	slots_of(world, roster[1])[uint16_t(65)] = w::PersonalWeaponSlot{7, 0};

	// --- 1. Valid primary fire: ring append + clip decrement + mirrors, no replies. ---
	const std::vector<uint8_t> body =
			fire_body(hb.packed, /*flags=*/0x22, /*adm=*/5, 0x10000, 0x8000, 0x18000,
	                  /*dx=*/0x1234, /*dy=*/int32_t(0xFFFFAAAA), hc.packed,
	                  /*hit_part=*/1025, /*extra2=*/12, /*misc=*/64);
	if (!expect(body.size() == 45, "crafted 0x06 body is 45 B"))
		return 1;
	int nreplies = dispatch_fire(roster[1], roster, world, body);
	if (!expect(nreplies == 0, "0x06 draws NO reactive reply")) return 1;
	if (!expect(world.out.rounds.count == 1, "one ring event appended")) return 1;
	{
		const w::RoundEvent &ev = world.out.rounds.records[0];
		if (!expect(ev.shooter_handle == hb.packed, "ring shooter = the connection's entity"))
			return 1;
		if (!expect(ev.origin_x == 0x10000 && ev.origin_y == 0x8000 && ev.origin_z == 0x18000,
		            "ring origin = the claimed fire origin (within the shooter's reach)"))
			return 1;
		if (!expect(ev.dir_yaw == int32_t(0x1234u << 16) &&
		                    ev.dir_pitch == int32_t(0xAAAAu << 16),
		            "ring direction = the raw dir words << 16"))
			return 1;
		if (!expect(ev.shot_seq == 1025, "ring shot_seq = the uplink hit_part")) return 1;
		if (!expect(ev.mode_flags == ((30 & 3) << 4 | 2),
		            "ring mode = the fire action's byte from the pre-spend clip")) return 1;
		if (!expect(ev.subtype == 12, "ring subtype = extra_byte2 composite")) return 1;
		if (!expect(ev.slot_byte == 64, "ring slot byte = PowerThrow charge")) return 1;
		if (!expect(ev.adm_index == 5, "ring adm index")) return 1;
	}
	{
		const uint16_t combo = 3 * 65 + 2;
		auto it = slots_of(world, roster[1]).find(combo);
		if (!expect(it != slots_of(world, roster[1]).end(), "the granted slot keeps its combo")) return 1;
		if (!expect(it->second.clip == 29, "clip 30 -> 29 after one primary fire")) return 1;
	}
	{
		const w::LiveRound &round = world.round_sim.rounds[0];
		const float speed =
				std::sqrt(round.vel.x * round.vel.x + round.vel.y * round.vel.y +
				          round.vel.z * round.vel.z);
		if (!expect(std::fabs(speed - (620.0f / 62.0f) * (64.0f / 256.0f)) < 0.01f,
		            "remote PowerThrow charge scales the authoritative round"))
			return 1;
	}
	{
		const w::Entity *sh = world.registry.get(hb);
		if (!expect(sh != nullptr && sh->equipped_adm_index == 5,
		            "equipped adm mirrored [orig: entity+688]"))
			return 1;
		if (!expect(sh->last_fire_target == hc, "claimed target stamped on the shooter"))
			return 1;
	}
	{
		std::vector<uint8_t> raw;
		ns::Datagram dg;
		if (!expect(!udp_b.pop_outbound(raw) && !udp_c.pop_outbound(raw) && !loop.client_recv(dg),
		            "no direct sends from the 0x06 dispatch"))
			return 1;
	}

	// --- 2. Spoofed shooter (C's handle on B's connection) -> dropped. ---
	dispatch_fire(roster[1], roster, world,
	              fire_body(hc.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 1, 12, 0));
	if (!expect(world.out.rounds.count == 1, "spoofed shooter handle dropped")) return 1;

	// --- 3. Unknown adm on an armory-fed host -> dropped [orig: NULL wpn -3]. ---
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 6, 0, 0, 0, 0, 0, 0xFFFF, 2, 12, 0));
	if (!expect(world.out.rounds.count == 1, "unknown adm dropped")) return 1;

	// --- 4. Alt fire appends WITHOUT touching the clip [orig: @0x50bb0d/@0x50be2b]. ---
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x23, 5, 0, 0, 0, 0, 0, 0xFFFF, 3, 12, 0));
	if (!expect(world.out.rounds.count == 2, "alt fire appended")) return 1;
	if (!expect(slots_of(world, roster[1])[uint16_t(3 * 65 + 2)].clip == 29,
	            "alt fire leaves the clip untouched"))
		return 1;

	// --- 5. The host's own loopback 0x06 is a no-op [orig: @0x50c18d]. ---
	dispatch_fire(roster[0], roster, world,
	              fire_body(ha.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 4, 12, 0));
	if (!expect(world.out.rounds.count == 2, "loopback fire ignored on the net path")) return 1;

	// --- 6. Clip exhaustion rejects; the 0x25 relay refills [orig: @0x541720]. ---
	slots_of(world, roster[1])[uint16_t(3 * 65 + 2)].clip = 1;
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 5, 12, 0));
	if (!expect(world.out.rounds.count == 3, "last round fires")) return 1;
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 6, 12, 0));
	if (!expect(world.out.rounds.count == 3, "empty clip rejects fire (NO AMMO)")) return 1;
	{
		// C2S 0x25 [u16 handle][u16 combo] from B -> relay + host-side refill.
		std::vector<uint8_t> reload;
		put_u16(reload, hb.packed);
		put_u16(reload, uint16_t(3 * 65 + 2));
		std::vector<ProtocolMessage> msgs;
		msgs.push_back(make_protocol_message(0x25, reload));
		inmatch::dispatch_session_replies(inmatch::GameConfig{}, roster[1], msgs, 101, roster, &world);
		// Drain the relayed 0x49s so later checks stay clean.
		std::vector<uint8_t> raw;
		ns::Datagram dg;
		while (udp_b.pop_outbound(raw)) {}
		while (udp_c.pop_outbound(raw)) {}
		while (loop.client_recv(dg)) {}
	}
	if (!expect(slots_of(world, roster[1])[uint16_t(3 * 65 + 2)].clip == 30,
	            "0x25 relay refilled the clip to capacity"))
		return 1;
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 7, 12, 0));
	if (!expect(world.out.rounds.count == 4, "fire works again after the reload")) return 1;

	// --- 7. A no-clip weapon (clipsize -1) never ammo-rejects [orig: adm+88 == -1]. ---
	for (int i = 0; i < 3; ++i)
		dispatch_fire(roster[1], roster, world,
		              fire_body(hb.packed, 0x12, 7, 0, 0, 0, 0, 0, 0xFFFF, uint16_t(10 + i),
		                        12, 0));
	if (!expect(world.out.rounds.count == 7, "no-clip weapon fires freely")) return 1;

	// --- 8. Table-less host accepts without bookkeeping (the 0x5A echo fallback shape). ---
	world.tables.weapons.entries.clear();
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 9, 0, 0, 0, 0, 0, 0xFFFF, 20, 12, 0));
	if (!expect(world.out.rounds.count == 8, "table-less host accepts the fire")) return 1;

	std::printf("OK\n");
	return 0;
}
