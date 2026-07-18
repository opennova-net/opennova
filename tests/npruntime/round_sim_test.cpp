// §5.60 — the authoritative round sim + damage + death routing. An accepted C2S 0x06
// spawns a live round SYNCHRONOUSLY with the ring append [orig: RoundData_AddRound
// @0x4fdb40 inline-calls RoundData_SpawnRound @0x4ec0d0]; Server_TickUpdate's world tick
// flies it [orig: Weapon_UpdateAllProjectiles @0x4ec020 -> Projectile_UpdatePhysics
// @0x4e9d70], the hit applies the KINETIC damage number [orig: Weapon_CalcImpactDamage
// @0x4ec920 — min(62*|vel|,1219) * weight_in_grains / 875, floored/capped by
// min/max_damage], clamped to remaining health [orig: @0x4e8064], and a health<=0 victim
// raises the death routing [orig: Entity_CheckAndProcessDeath @0x51b550]: S2C 0x13
// [u16 victim][u16 killerSource] to every non-host in-match connection + the S2C 0x1E
// kill-feed event for player victims; a dead HOST player enters the respawn queue and
// releases back to its spawn point at template health [orig: Entity_ResetToSpawnState
// @0x4b9610].
//
// Coverage: build_ammo_table + round_type resolve; fire -> one live round with the
// velocity/62 step; three body hits kill a 150-hp player (60/60/30 clamped); 0x13 + 0x1E
// staged on both client transports, none on the loopback; a client-owned victim does NOT
// auto-respawn; a dead loopback (host) victim respawns after the timer at spawn health.

#include <npruntime/ammo_table_build.h>
#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_server_ctx.h>
#include <npruntime/server_message_dispatch.h>
#include <npruntime/server_tick.h>

#include <netsim/connection.h>
#include <netsim/loopback_channel.h>
#include <netsim/session_transport.h>
#include <netsim/udp_session_transport.h>

#include <npwire/protocol_message.h>
#include <npwire/replication_model.h>

#include <terrain/height_field.h>

#include <world/ai.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

w::PlayerSpawn player_spawn(uint16_t net_id, float x, float y, float z) {
	w::PlayerSpawn s;
	s.position = {x, y, z};
	s.net_id = net_id;
	return s;
}

np::NapiNPConnection make_conn(uint32_t id, int type, ns::ISessionTransport *t,
                               ns::TransportMode mode, w::EntityHandle owned, bool spawned) {
	np::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = spawned;
	c.spawned_announced = spawned;
	c.phase = spawned ? np::ConnectionPhase::InMatch : np::ConnectionPhase::New;
	return c;
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

// The fixed 45-B C2S 0x06 body (§5.16 field order).
std::vector<uint8_t> fire_body(uint16_t shooter, uint8_t adm, int32_t px, int32_t py,
                               int32_t pz, int32_t dx, int32_t dy) {
	std::vector<uint8_t> b;
	put_u32(b, 12345);
	put_u16(b, shooter);
	b.push_back(0x02); // primary fire
	b.push_back(adm);
	put_u32(b, uint32_t(px));
	put_u32(b, uint32_t(py));
	put_u32(b, uint32_t(pz));
	put_u32(b, uint32_t(dx));
	put_u32(b, uint32_t(dy));
	put_u16(b, 0xFFFF); // no claimed target
	put_u16(b, 513);
	b.push_back(0x07);
	b.push_back(0x0c);
	b.push_back(0x00);
	put_u16(b, 0);
	put_u16(b, 0);
	put_u16(b, 0);
	put_u16(b, 0);
	put_u16(b, 0);
	return b;
}

void dispatch_fire(np::NapiNPConnection &conn, std::vector<np::NapiNPConnection> &roster,
                   w::World &world, const std::vector<uint8_t> &body) {
	std::vector<ProtocolMessage> msgs;
	msgs.push_back(make_protocol_message(0x06, body));
	np::dispatch_session_replies(np::GameConfig{}, conn, msgs, 100, roster, &world);
}

// Drain ALL staged S2C datagrams (0x0A noise included) once; pick tags from the result.
struct Drained {
	std::vector<std::vector<uint8_t>> raw;
	std::vector<std::vector<uint8_t>> tag(uint8_t want) const {
		std::vector<std::vector<uint8_t>> out;
		for (const auto &d : raw)
			if (!d.empty() && d[0] == want) out.emplace_back(d.begin() + 1, d.end());
		return out;
	}
};
Drained drain_all(ns::UdpSessionTransport &t) {
	Drained out;
	std::vector<uint8_t> raw;
	while (t.pop_outbound(raw)) out.raw.push_back(raw);
	return out;
}

} // namespace

int main() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	world.player_item_hp = 150; // items.def Player hp (D-NET-144)

	// Host player down-range past the victim on the same +X line; shooter at the origin.
	const w::EntityHandle ha = w::spawn_player(world, player_spawn(0xFFF0, 60.0f, 0.0f, 10.0f));
	const w::EntityHandle hb =
			w::spawn_remote_player(world, player_spawn(0xFFF1, 0.0f, 0.0f, 10.0f)); // shooter
	const w::EntityHandle hc =
			w::spawn_remote_player(world, player_spawn(0xFFF2, 30.0f, 0.0f, 10.0f)); // victim
	if (!expect(ha.valid() && hb.valid() && hc.valid(), "three players spawned")) return 1;
	if (!expect(world.registry.get(hc)->health == 150, "victim spawns at template hp 150"))
		return 1;

	// Armory: adm 5 = a rifle firing TEST_556. Ammo table via the real builder — entry 0
	// is the file-order null, entry 1 the live round (854 u/s, 62 grains, 3 s, C4 kz —
	// the real 5.56 shape).
	world.weapons.entries.resize(8);
	{
		w::WeaponTableEntry &rifle = world.weapons.entries[5];
		rifle.name = "WPN_TESTRIFLE";
		rifle.category = 3;
		rifle.rank = 2;
		rifle.clipsize = 30;
		rifle.round_type = "TEST_556";
		rifle.valid = true;
	}
	{
		DefAmmoFile file;
		std::memset(&file, 0, sizeof(file));
		static DefAmmoDef defs[2];
		std::memset(defs, 0, sizeof(defs));
		std::snprintf(defs[0].name, sizeof(defs[0].name), "AT_NULL");
		std::snprintf(defs[1].name, sizeof(defs[1].name), "TEST_556");
		defs[1].velocity = 854;
		defs[1].weight_in_grains = 62;
		defs[1].max_age_ticks = 186;
		defs[1].kztype = DEF_AMMO_KZ_C4;
		defs[1].bullet_radius_fp16 = 182;
		// The per-surface impact rows (the real AMMO_AK47_556MM shape): the bake maps
		// tag names to the canonical table slots, keeps the FIRST duplicate, empties
		// 'none' columns, and discards the count column [orig: effects_table stage
		// @ 0x40a46a; count discard @ 0x40a587; AmmoDef_InitEffectsTable @ 0x409f20].
		static DefEffectTableEntry fx_rows[4];
		std::memset(fx_rows, 0, sizeof(fx_rows));
		std::snprintf(fx_rows[0].surface_type, sizeof(fx_rows[0].surface_type), "dirt");
		std::snprintf(fx_rows[0].hit_effect, sizeof(fx_rows[0].hit_effect), "Effect_AmHitDirt");
		std::snprintf(fx_rows[0].impact_sound, sizeof(fx_rows[0].impact_sound), "IMP_BULLET_DIRT");
		fx_rows[0].value = 15;
		std::snprintf(fx_rows[1].surface_type, sizeof(fx_rows[1].surface_type), "Player");
		std::snprintf(fx_rows[1].hit_effect, sizeof(fx_rows[1].hit_effect), "Effect_AmHitBody");
		std::snprintf(fx_rows[1].impact_sound, sizeof(fx_rows[1].impact_sound), "IMP_BULLET_PLAYER");
		fx_rows[1].value = 10;
		std::snprintf(fx_rows[2].surface_type, sizeof(fx_rows[2].surface_type), "zip");
		std::snprintf(fx_rows[2].hit_effect, sizeof(fx_rows[2].hit_effect), "none");
		std::snprintf(fx_rows[2].impact_sound, sizeof(fx_rows[2].impact_sound), "WSH_BULLET_BY");
		fx_rows[2].value = 10;
		std::snprintf(fx_rows[3].surface_type, sizeof(fx_rows[3].surface_type), "dirt"); // dup
		std::snprintf(fx_rows[3].hit_effect, sizeof(fx_rows[3].hit_effect), "Effect_WRONG");
		std::snprintf(fx_rows[3].impact_sound, sizeof(fx_rows[3].impact_sound), "none");
		defs[1].effects_table = fx_rows;
		defs[1].effects_table_count = 4;
		file.entries = defs;
		file.count = 2;
		world.ammo = np::build_ammo_table(file);
		defs[1].effects_table = nullptr; // static rows; keep def_free-style cleanup moot
		defs[1].effects_table_count = 0;
		np::resolve_weapon_round_types(world.weapons, world.ammo);
	}
	if (!expect(world.weapons.entries[5].ammo_index == 1, "round_type resolved to ammo 1"))
		return 1;
	{
		// The baked rows land in the canonical tag slots [orig: g_AmmoEffectTagTable
		// @ 0x813420 — player=2, dirt=5, zip=3].
		const w::AmmoTableEntry *a = world.ammo.by_index(1);
		if (!expect(a != nullptr, "ammo 1 valid")) return 1;
		if (!expect(a->impact_effects[5].effect == "Effect_AmHitDirt" &&
		                    a->impact_effects[5].sound == "IMP_BULLET_DIRT",
		            "dirt row baked at tag 5 (first duplicate wins)"))
			return 1;
		if (!expect(a->impact_effects[2].effect == "Effect_AmHitBody",
		            "case-insensitive 'Player' tag baked at 2"))
			return 1;
		if (!expect(a->impact_effects[3].effect.empty() &&
		                    a->impact_effects[3].sound == "WSH_BULLET_BY",
		            "'none' effect column stays empty; the sound still bakes"))
			return 1;
		if (!expect(a->impact_effects[11].effect.empty() && a->impact_effects[11].sound.empty(),
		            "unauthored tags stay empty"))
			return 1;
	}

	ns::LoopbackChannel loop;
	ns::UdpSessionTransport udp_b(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport udp_c(ns::UdpSessionTransport::Role::Host);

	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	// This scenario is an MP session (three players over transports): stamp the
	// world-side flag too, or the SP-only round-outcome legs (kill tallies + the
	// death auto-lose in check_win_conditions) run and hold the respawn queue.
	world.mp_session = true;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_conn(1, 2, &loop, ns::TransportMode::Loopback, ha, true));
	roster.push_back(make_conn(3, 1, &udp_b, ns::TransportMode::Client, hb, true));
	roster.push_back(make_conn(4, 1, &udp_c, ns::TransportMode::Client, hc, true));

	const PlayerReplicationState anchor{};

	// --- 1. Fire spawns one live round with the witnessed velocity step. ---
	// Wire yaw BAM 0 -> mission bearing 0 = +X: the 0x06 yaw IS the mission bearing
	// (v29 wire-validated; D-NET-153). Muzzle at torso height (hit spheres at z + 0.9).
	const int32_t muzzle_z = int32_t((10.0 + 0.9) * 65536.0);
	dispatch_fire(roster[1], roster, world, fire_body(hb.packed, 5, 0, 0, muzzle_z, 0, 0));
	if (!expect(world.round_sim.active_count == 1, "one live round after the fire")) return 1;
	{
		const w::LiveRound &r = world.round_sim.rounds[0];
		if (!expect(r.active && r.ammo_index == 1, "round bound to the resolved ammo"))
			return 1;
		const float speed = std::sqrt(r.vel.x * r.vel.x + r.vel.y * r.vel.y + r.vel.z * r.vel.z);
		if (!expect(std::fabs(speed - 854.0f / 62.0f) < 0.01f,
		            "round speed = ammo velocity / 62 per tick [orig: @0x4ec508]"))
			return 1;
		if (!expect(r.vel.x > 13.0f && std::fabs(r.vel.y) < 0.1f && std::fabs(r.vel.z) < 0.1f,
		            "wire yaw BAM 0 flies +X (bearing = wire yaw; D-NET-153)"))
			return 1;
	}

	// --- 2. Three ticks reach the victim at x=30; the hit applies the kinetic number:
	// min(62*13.77, 1219)=854 -> 854*62/875 = 60. ---
	for (int i = 0; i < 3; ++i) np::Server_TickUpdate(ctx, anchor);
	if (!expect(world.round_sim.active_count == 0, "round consumed by the hit")) return 1;
	if (!expect(world.registry.get(hc)->health == 90, "150 - 60 kinetic damage = 90")) return 1;
	// The hit queued ONE impact for the presenting host: tag 2 'player' (pool-0
	// organics), direction = the normalized flight ray, position on the hit sphere
	// short of the victim at x=30 [orig: Projectile_HandleEntityImpact ->
	// Projectile_SpawnImpactEffect @ 0x4e9b80].
	if (!expect(world.round_sim.impacts.size() == 1, "one impact queued for the hit")) return 1;
	{
		const w::RoundImpact &imp = world.round_sim.impacts[0];
		if (!expect(imp.effect_tag == 2, "entity hit selects tag 2 'player'")) return 1;
		if (!expect(imp.ammo_index == 1, "impact carries the round's ammo index")) return 1;
		if (!expect(std::fabs(imp.direction.x - 1.0f) < 0.01f, "impact direction = +X flight"))
			return 1;
		if (!expect(imp.position.x > 27.0f && imp.position.x < 30.5f,
		            "impact position lands at the victim's hit sphere"))
			return 1;
	}
	world.round_sim.impacts.clear(); // the presenter drain, stubbed

	// --- 2b. Terrain impact: a missed shot stops ON the surface with the dirt tag.
	// Flat synthetic heightfield (ground = 0 everywhere); the round flies down at
	// -45 deg from z=+5 on an empty lane (y=50, no entities). The impact-effect tag
	// is the no-surface-map default (type 1 + 4 = dirt) and the interpolated stop
	// sits on the plane, not a sub-step under it
	// [orig: Projectile_HandleTerrainImpact -> Projectile_SpawnImpactEffect
	//  @ 0x4e9b80; D-WPN-15 carries the remaining tag-selection gaps]. ---
	{
		std::vector<uint16_t> flat_hm(512 * 512, 0);
		std::vector<int> flat_grid(256, 1);
		opennova::terrain::TerrainHeightField field;
		field.heightmap = flat_hm.data();
		field.dim = 512;
		field.layout.sector_grid = flat_grid.data();
		field.layout.origin_x = 0;
		field.layout.origin_y = 0;

		w::RoundSpawnParams params;
		params.origin = {0.0f, 50.0f, 5.0f};
		params.dir_yaw_bam = 0;                    // +X
		params.dir_pitch_bam = int32_t(0xE0000000); // -45 deg
		params.ammo_index = 1;
		if (!expect(world.round_sim.spawn(world, params) >= 0, "terrain-leg round spawned"))
			return 1;
		for (int i = 0; i < 4 && world.round_sim.active_count > 0; ++i)
			world.round_sim.tick(world, &field, nullptr);
		if (!expect(world.round_sim.active_count == 0, "terrain stopped the round")) return 1;
		if (!expect(world.round_sim.impacts.size() == 1, "one terrain impact queued")) return 1;
		const w::RoundImpact &imp = world.round_sim.impacts[0];
		if (!expect(imp.effect_tag == 1 + 4, "terrain hit takes the no-map dirt tag [orig: @ 0x4e8862]"))
			return 1;
		if (!expect(std::fabs(imp.position.z) < 0.02f,
		            "the interpolated stop sits ON the surface, not a sub-step under it"))
			return 1;
		if (!expect(imp.direction.z < -0.5f && imp.direction.x > 0.5f,
		            "impact direction = the normalized downward flight ray"))
			return 1;
		world.round_sim.impacts.clear();
	}

	// The processed hit also writes the sticky SHOT relations (players carry
	// group 0, so only the single rows land; rows outside the retail < 0x80
	// guard are no-ops) [orig: Projectile_ProcessDamageOnTarget
	// @ 0x4e80ae..0x4e80ef; guard e.g. EntityMatrix_SetProximityBit @ 0x452b60].
	{
		const w::Entity *sh = world.registry.get(hb);
		const w::Entity *vic = world.registry.get(hc);
		const bool in_range = sh->net_id < 128 && vic->net_id < 128;
		if (!expect(world.relations.single_single(w::TriggerRelations::kShot,
		            sh->net_id, vic->net_id) == in_range,
		            "shot S->S written on processed damage iff rows pass the <0x80 guard"))
			return 1;
	}

	// --- 3. Two more hits kill: 90 -> 30 -> 0 (the last clamped to remaining health
	// [orig: @0x4e8064]); the death routes 0x13 + 0x1E to both clients, not the host. ---
	drain_all(udp_b); // clear the 0x0A noise so the death drain reads clean
	drain_all(udp_c);
	for (int shot = 0; shot < 2; ++shot) {
		dispatch_fire(roster[1], roster, world,
		              fire_body(hb.packed, 5, 0, 0, muzzle_z, 0, 0));
		for (int i = 0; i < 4; ++i) np::Server_TickUpdate(ctx, anchor);
	}
	if (!expect(world.registry.get(hc)->health == 0, "victim dead at 0 hp (clamped)")) return 1;
	const Drained after_kill_b = drain_all(udp_b);
	const Drained after_kill_c = drain_all(udp_c);
	{
		auto notif_b = after_kill_b.tag(0x13);
		auto notif_c = after_kill_c.tag(0x13);
		if (!expect(notif_b.size() == 1 && notif_c.size() == 1,
		            "one S2C 0x13 death notify per client"))
			return 1;
		const std::vector<uint8_t> &n = notif_b[0];
		if (!expect(n.size() == 4, "0x13 body is 4 B [u16 victim][u16 killerSource]"))
			return 1;
		const uint16_t victim = uint16_t(n[0] | (n[1] << 8));
		const uint16_t killer = uint16_t(n[2] | (n[3] << 8));
		if (!expect(victim == hc.packed && killer == hb.packed,
		            "0x13 carries victim + killer handles"))
			return 1;
	}
	{
		auto feed_b = after_kill_b.tag(0x1E);
		if (!expect(feed_b.size() == 1, "one S2C 0x1E kill-feed event")) return 1;
		const std::vector<uint8_t> &f = feed_b[0];
		if (!expect(f.size() == 8, "0x1E body is 8 B (§5.26)")) return 1;
		if (!expect(f[0] == 4, "standard-kill event_type 4")) return 1;
		if (!expect(f[1] == uint8_t(hb.packed & 0xFF) && f[2] == uint8_t(hc.packed & 0xFF),
		            "0x1E attacker/victim pool-0 index bytes"))
			return 1;
		const int16_t px = int16_t(f[4] | (f[5] << 8));
		const int16_t py = int16_t(f[6] | (f[7] << 8));
		if (!expect(px == 30 && py == 0, "0x1E event position in metres")) return 1;
	}
	if (!expect(ctx.respawn_queue.empty(), "a client-owned victim does NOT auto-respawn"))
		return 1;

	// --- 4. Kill the HOST player (loopback-owned, past the dead victim on the same
	// line): it queues for respawn and releases at spawn health/position. ---
	for (int shot = 0; shot < 3; ++shot) {
		dispatch_fire(roster[1], roster, world,
		              fire_body(hb.packed, 5, 0, 0, muzzle_z, 0, 0));
		for (int i = 0; i < 6; ++i) np::Server_TickUpdate(ctx, anchor);
	}
	if (!expect(world.registry.get(ha)->health == 0, "host player dead")) return 1;
	if (!expect(ctx.respawn_queue.size() == 1, "host player queued for respawn")) return 1;
	{
		// Displace the corpse to prove the release snaps back [orig: the D-NET-66
		// death/respawn teleport].
		w::Entity *host = world.registry.get(ha);
		host->position.x = 12.0f;
		for (int i = 0; i < 621; ++i) np::Server_TickUpdate(ctx, anchor);
		if (!expect(ctx.respawn_queue.empty(), "respawn released after the timer")) return 1;
		host = world.registry.get(ha);
		if (!expect(host->health == 150, "respawn restores template health")) return 1;
		if (!expect(std::fabs(host->position.x - 60.0f) < 0.01f,
		            "respawn snaps to the spawn point"))
			return 1;
	}

	// --- 5. The 0x0E deploy of a RESPAWN-PENDING joiner emits the DEPLOY-RELEASE bundle
	// (0x5A + 0x61 + optional 0x1E) and clears the pending/hidden pair — the client's 0x5A
	// apply resets its dword_81474C wait-gate (set by the pick) and resumes the C2S 0x0C
	// uplink [orig: Server_ProcessPlayerDeath deploy tail: Server_SendWeaponSlotListToPlayer
	// @0x502550 + Server_SendRandomSeedToPlayer @0x5101a0; client un-latch §5.30
	// @0x4290E0; golden deploy frame 240018 = 0x5A + 0x61 + 0x1E one datagram; the v32
	// rubber-band]. (D-NET-156 tail) ---
	{
		const w::EntityHandle jb = w::spawn_remote_player(world, player_spawn(0xFFF3, 5, 5, 0));
		if (!expect(jb.valid(), "deploy-test joiner spawned")) return 1;
		np::NapiNPConnection conn =
				make_conn(7, 1, &udp_b, ns::TransportMode::Client, jb, /*spawned=*/true);
		conn.link.respawn_pending = true;
		w::Entity *je = world.registry.get(jb);
		je->flags |= 1u; // the join-time hidden bit rides with pending
		conn.reply.last_loadout_reply = {8, 2, 255, 0, 0, 0xFF}; // a granted 0x5A body

		std::vector<ProtocolMessage> msgs;
		msgs.push_back(make_protocol_message(0x0E, {0xFF, 0xFF})); // param-0 pick (base deploy)
		std::vector<ProtocolMessage> replies = np::dispatch_session_replies(
				np::GameConfig{}, conn, msgs, 100, roster, &world, 0xA1B2C3D4u);

		bool saw_5a = false, saw_61 = false;
		for (const ProtocolMessage &m : replies) {
			if (m.tag == 0x5A) {
				saw_5a = true;
				if (!expect(m.payload == conn.reply.last_loadout_reply,
				            "deploy 0x5A re-sends the GRANTED loadout body"))
					return 1;
			}
			if (m.tag == 0x61) {
				saw_61 = true;
				if (!expect(m.payload.size() == 4 && m.payload[0] == 0xD4 &&
				                    m.payload[3] == 0xA1,
				            "deploy 0x61 carries the session seed"))
					return 1;
			}
		}
		if (!expect(saw_5a, "deploy release emits the 0x5A un-latcher")) return 1;
		if (!expect(saw_61, "deploy release emits the 0x61 seed")) return 1;
		if (!expect(!conn.link.respawn_pending, "deploy clears respawn_pending")) return 1;
		je = world.registry.get(jb);
		if (!expect((je->flags & 1u) == 0, "deploy clears the hidden bit")) return 1;
		if (!expect(je->health > 0, "deploy restores health")) return 1;

		// An alive DEPLOYED player's 0x0E is a no-op (the dead-or-pending gate @0x519cc7):
		// no bundle, no reposition.
		std::vector<ProtocolMessage> again = np::dispatch_session_replies(
				np::GameConfig{}, conn, msgs, 101, roster, &world, 0xA1B2C3D4u);
		for (const ProtocolMessage &m : again)
			if (!expect(m.tag != 0x5A && m.tag != 0x61,
			            "alive deployed 0x0E draws no release bundle"))
				return 1;
	}

	// --- 6. The tracer decision [orig: RoundData_SpawnRound @0x4ec184-0x4ec1e5]:
	// every tracer_rate-th round per shooter is a tracer (the counter wraps at the
	// rate; tracer on wrap), rate 0 = never, FORCETRACER (flags 0x8000) = every
	// round; team is stamped from the shooter [orig: round+0x162 @0x4ec705]. Every
	// spawn also records a FireEvent for the host present drain (§17.4).
	{
		world.ammo.entries[1].tracer_rate = 3;
		w::Entity *shooter = world.registry.get(hb);
		shooter->tracer_shot_counter = 0;
		world.round_sim.fired.clear();
		w::RoundSpawnParams rp;
		rp.owner = hb;
		rp.shooter_handle = hb.packed;
		rp.origin = {0.0f, 0.0f, 30.0f};
		rp.ammo_index = 1;
		for (int shot = 1; shot <= 6; ++shot) {
			const int slot = world.round_sim.spawn(world, rp);
			if (!expect(slot >= 0, "tracer-cadence round spawned")) return 1;
			const bool want = (shot % 3) == 0; // counter wrap = every 3rd shot
			if (!expect(world.round_sim.rounds[size_t(slot)].tracer == want,
			            "tracer cadence: tracer exactly on the counter wrap"))
				return 1;
			if (!expect(world.round_sim.rounds[size_t(slot)].team == shooter->team,
			            "tracer round carries the shooter team"))
				return 1;
		}
		world.ammo.entries[1].tracer_rate = 0;
		int s0 = world.round_sim.spawn(world, rp);
		if (!expect(s0 >= 0 && !world.round_sim.rounds[size_t(s0)].tracer,
		            "tracer_rate 0 -> never a tracer"))
			return 1;
		world.ammo.entries[1].flags |= 0x8000u; // forcetracer
		int s1 = world.round_sim.spawn(world, rp);
		if (!expect(s1 >= 0 && world.round_sim.rounds[size_t(s1)].tracer,
		            "FORCETRACER overrides rate 0"))
			return 1;
		world.ammo.entries[1].flags &= ~0x8000u;
		if (!expect(world.round_sim.fired.size() == 8 &&
		                    world.round_sim.fired.back().ammo_index == 1 &&
		                    world.round_sim.fired.back().shooter_handle == hb.packed,
		            "every spawn records a FireEvent for the present drain"))
			return 1;
		world.round_sim.fired.clear();
	}

	// --- 7. The tracer trail channels [orig: g_TracerEmitterPool @ 0x2BF5270 — alloc
	// at spawn with the friendly/enemy style vs the local team @ 0x4ec740, one
	// pre-move point per tick (Projectile_UpdatePhysics @ 0x4ea97a), ring-capped at
	// the style count, death append + drain (Projectile_ReleaseEffects @ 0x4e8280 ->
	// CEffectEmitterPool_Tick @ 0x5db830: cap-length grace, then one pop per tick)].
	{
		auto &sim = world.round_sim;
		auto &ammo1 = world.ammo.entries[1];
		ammo1.tracer_rate = 1; // every round a tracer
		ammo1.tracer_type_friendly = 1;
		ammo1.tracer_type_enemy = 2;
		w::Entity *shooter = world.registry.get(hb);
		shooter->tracer_shot_counter = 0;
		sim.local_player = hb; // shooter == the presenting player -> friendly
		sim.local_team = static_cast<uint8_t>(shooter->team);
		w::RoundSpawnParams rp;
		rp.owner = hb;
		rp.shooter_handle = hb.packed;
		rp.ammo_index = 1;
		rp.origin = {0.0f, 0.0f, 500.0f}; // high above every organic + no terrain
		const int slot = sim.spawn(world, rp);
		if (!expect(slot >= 0 && sim.rounds[size_t(slot)].trail_slot >= 0,
		            "a tracer round allocates a trail channel at spawn"))
			return 1;
		const int ch_i = sim.rounds[size_t(slot)].trail_slot;
		auto &ch = sim.trails.channels[size_t(ch_i)];
		if (!expect(ch.style_id == 1, "shooter == local player selects the friendly style"))
			return 1;
		if (!expect(ch.cap == 12, "stdred ring cap = the witnessed 12-entry table"))
			return 1;
		for (int t = 0; t < 5; ++t) sim.tick(world, nullptr, nullptr);
		if (!expect(ch.count == 5, "one trail point per tick while alive")) return 1;
		if (!expect(ch.pts[0].pos.x == 0.0f && ch.pts[0].pos.z == 500.0f,
		            "the first point is the PRE-move spawn origin"))
			return 1;
		if (!expect(ch.pts[0].w == 1.0f, "std styles carry no width jitter")) return 1;
		if (!expect(ch.age == 1, "a live channel's age re-arms every append")) return 1;
		// Death by age-out: final point + kill request, then the drain timeline.
		sim.rounds[size_t(slot)].max_age_ticks = sim.rounds[size_t(slot)].age_ticks;
		sim.tick(world, nullptr, nullptr);
		if (!expect(!sim.rounds[size_t(slot)].active && ch.kill && ch.count == 6,
		            "round death appends the final point and requests the drain"))
			return 1;
		for (int t = 0; t < 11; ++t) sim.tick(world, nullptr, nullptr);
		if (!expect(ch.active && ch.count == 6,
		            "the dead trail holds shape through the cap-length grace"))
			return 1;
		for (int t = 0; t < 30; ++t) sim.tick(world, nullptr, nullptr);
		if (!expect(!ch.active, "the drained channel frees its slot")) return 1;

		// Enemy select: a presenting client on another team gets the enemy style.
		sim.local_player = w::EntityHandle{};
		sim.local_team = 99;
		const int slot_e = sim.spawn(world, rp);
		if (!expect(slot_e >= 0 && sim.rounds[size_t(slot_e)].trail_slot >= 0 &&
		                    sim.trails.channels[size_t(sim.rounds[size_t(slot_e)].trail_slot)]
		                                    .style_id == 2,
		            "a team mismatch selects the enemy style"))
			return 1;
		sim.rounds[size_t(slot_e)].active = false;
		--sim.active_count;

		// Ring cap: a long flight tops out at the style's point count.
		sim.local_team = static_cast<uint8_t>(shooter->team);
		const int slot_r = sim.spawn(world, rp);
		if (!expect(slot_r >= 0 && sim.rounds[size_t(slot_r)].trail_slot >= 0,
		            "ring-cap round spawned"))
			return 1;
		auto &ch_r = sim.trails.channels[size_t(sim.rounds[size_t(slot_r)].trail_slot)];
		for (int t = 0; t < 20; ++t) sim.tick(world, nullptr, nullptr);
		if (!expect(ch_r.count == 12, "the ring caps at the style count (oldest drops)"))
			return 1;
		sim.rounds[size_t(slot_r)].active = false;
		--sim.active_count;

		// The MP NoTracers rules bit kills the visual unless FORCETRACER
		// [orig: dword_24D1E34 & 1 gate @ 0x4ec740 / forcetracer bypass].
		sim.no_tracers_rule = true;
		const int slot_n = sim.spawn(world, rp);
		if (!expect(slot_n >= 0 && sim.rounds[size_t(slot_n)].trail_slot < 0 &&
		                    sim.rounds[size_t(slot_n)].tracer,
		            "NoTracers keeps the sim tracer flag but spawns no channel"))
			return 1;
		sim.rounds[size_t(slot_n)].active = false;
		--sim.active_count;
		ammo1.flags |= 0x8000u; // forcetracer
		const int slot_f = sim.spawn(world, rp);
		if (!expect(slot_f >= 0 && sim.rounds[size_t(slot_f)].trail_slot >= 0,
		            "FORCETRACER bypasses NoTracers for the visual"))
			return 1;
		ammo1.flags &= ~0x8000u;
		sim.no_tracers_rule = false;
		sim.rounds[size_t(slot_f)].active = false;
		--sim.active_count;
		sim.trails.reset();
		sim.fired.clear();
	}

	std::printf("round_sim_test: all green\n");
	return 0;
}
