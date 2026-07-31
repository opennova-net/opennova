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
#include <netsim/net_client_view.h>
#include <netsim/session_transport.h>
#include <netsim/udp_session_transport.h>

#include <npwire/protocol_message.h>
#include <npwire/replication_model.h>

#include <terrain/height_field.h>

#include <world/ai.h>
#include <world/geom.h>
#include <world/infantry.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include <cmath>
#include <array>
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

struct DeathAnimSource final : w::IRootMotionSource {
	bool has_clip(int, int state_id) const override {
		return state_id == w::anim_state::kIdle ||
		       state_id == w::anim_state::kIdle2 ||
		       (state_id >= w::anim_state::kDeathFire &&
		        state_id <= w::anim_state::kDeathBulletBase + 59);
	}
	int32_t clip_length_ticks(int, int) const override { return -1; }
	bool advance(int, int state_id, int32_t &phase, w::RootMotionFrame &out) override {
		if (!has_clip(0, state_id)) return false;
		++phase;
		out = w::RootMotionFrame{};
		return true;
	}
};

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

void deliver_all(const Drained &drained, ns::UdpSessionTransport &client) {
	for (const std::vector<uint8_t> &raw : drained.raw) client.push_inbound(raw);
}

bool test_retail_random_spread_vectors() {
	struct Vector {
		int32_t spread;
		uint32_t seed;
		int32_t vertical;
		bool alternate;
		int32_t yaw;
		int32_t pitch;
	};
	// Independent IDA/x87 oracle vectors. In particular, strict-f32
	// intermediates miss several of these by 1-4 BAM units.
	// [orig: Weapon_CalcRandomSpreadOffset @0x4E4120]
	constexpr std::array<Vector, 11> vectors{{
		{0x00000000, 0xFFFFFFFFu, 0x00010000, false, 0, 0},
		{0x00010000, 0x00000000u, 0, false, -7340450, 6771822},
		{0x00010000, 0x12345678u, 0, false, -4357031, 7278683},
		{0x00010000, 0x12345678u, 0x00008000, false, -4357031, 3639341},
		{0x00010000, 0x12345678u, 0, true, 9357827, -5601610},
		{0x00010000, 0x12345678u, 0x00008000, true, 9357827, 66155},
		{0x00028000, 0x80000000u, 0x00008000, false, -20294730, 3391413},
		{0x00028000, 0x80000000u, 0x00008000, true, 16461567, 1713528},
		{0x00004000, 0xFFFFFFFFu, 0, false, 1199147, 1599490},
		{0x00004000, 0xFFFFFFFFu, 0x00018000, true, 244338, -1924904},
		{static_cast<int32_t>(0xFFFF0000u), 0xDEADBEEFu, 0, false, 225733, -8847762},
	}};
	for (const Vector &v : vectors) {
		const w::RandomSpreadOffset got = w::weapon_calc_random_spread_offset(
				v.spread, v.seed, v.vertical, v.alternate);
		if (!expect(got.yaw_bam == v.yaw && got.pitch_bam == v.pitch,
		            "retail random-spread vector matches IDA/x87 oracle")) {
			std::fprintf(stderr,
			             "  spread=%08X seed=%08X vertical=%08X alt=%d got=(%d,%d) expected=(%d,%d)\n",
			             static_cast<uint32_t>(v.spread), v.seed,
			             static_cast<uint32_t>(v.vertical), v.alternate ? 1 : 0,
			             got.yaw_bam, got.pitch_bam, v.yaw, v.pitch);
			return false;
		}
	}

	struct ShotgunVector {
		int32_t pie_slice;
		uint16_t radial;
		uint16_t phase;
		int32_t yaw;
		int32_t pitch;
	};
	static constexpr ShotgunVector shotgun_vectors[] = {
		{0x01000000, 12695, 50206, 1614661, -15924875},
		{0x00100000, 1, 32785, -1048573, -1706},
		{0x00280000, 65518, 32478, -1133, 31},
		{0x00010000, 28826, 12516, 18303, 47072},
	};
	for (const ShotgunVector &v : shotgun_vectors) {
		const w::RandomSpreadOffset got =
				w::weapon_calc_shotgun_spread_offset(
						v.pie_slice, v.radial, v.phase);
		if (!expect(got.yaw_bam == v.yaw && got.pitch_bam == v.pitch,
		            "retail shotgun radial-spread vector matches IDA/x87 oracle"))
			return false;
	}
	return true;
}

bool test_spawn_spread_then_recoil() {
	w::World world;
	world.registry.configure_pool(0, 4);
	w::AiSystem ai;
	world.ai = &ai;
	w::PlayerSpawn seed;
	seed.net_id = 41;
	seed.equipped_adm_index = 1;
	const w::EntityHandle shooter = w::spawn_player(world, seed);
	w::AiEntity *body = ai.for_handle(shooter);
	if (!expect(shooter.valid() && body != nullptr,
	            "spread/recoil integration player spawned"))
		return false;

	world.ammo.entries.resize(1);
	w::AmmoTableEntry &ammo = world.ammo.entries[0];
	ammo.valid = true;
	ammo.velocity = 620;
	ammo.max_age_ticks = 100;
	ammo.recoil[0] = 1;
	ammo.recoil[1] = 2;
	ammo.recoil[2] = 3;
	world.weapons.entries.resize(2);
	w::WeaponTableEntry &weapon = world.weapons.entries[1];
	weapon.valid = true;
	weapon.error_fp16[2] = 0x10000;

	w::RoundSpawnParams params;
	params.owner = shooter;
	params.shooter_handle = shooter.packed;
	params.ammo_index = 0;
	params.adm_index = 1;
	params.shot_seq = 0;
	const w::RandomSpreadOffset first_error =
			w::weapon_calc_random_spread_offset(0x10000, 0, 0, false);
	const int first = world.round_sim.spawn(world, params);
	if (!expect(first >= 0, "ordinary weapon round spawned")) return false;
	if (!expect(world.round_sim.rounds[static_cast<size_t>(first)].yaw_bam ==
	                    first_error.yaw_bam &&
	                    world.round_sim.rounds[static_cast<size_t>(first)].pitch_bam ==
	                    first_error.pitch_bam,
	            "current shot uses static ERROR before adding its recoil"))
		return false;
	if (!expect(body->inf.recoil_pitch == (3 << 18),
	            "standing ammo recoil is added after successful spawn"))
		return false;
	if (!expect(world.round_sim.fired.size() == 1 &&
	                    world.round_sim.fired[0].yaw_bam == 0 &&
	                    world.round_sim.fired[0].pitch_bam == 0,
	            "fire descriptor remains pre-random-spread"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	body->inf.scope_raised = true;
	world.registry.get(shooter)->flags |= w::kEntityFlagScopeRaised;
	const int scoped = world.round_sim.spawn(world, params);
	if (!expect(scoped >= 0 && body->inf.recoil_pitch == ((3 << 18) * 3 / 4),
	            "scope-raised recoil applies the exact binary32 0.75 scale"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	body->inf.scope_raised = false;
	world.registry.get(shooter)->flags &= ~w::kEntityFlagScopeRaised;
	world.round_sim.weapon_spread_enabled = false;
	const int gated = world.round_sim.spawn(world, params);
	if (!expect(gated >= 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(gated)].yaw_bam == 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(gated)].pitch_bam == 0 &&
	                    body->inf.recoil_pitch == (3 << 18),
	            "weapon rules gate suppresses ERROR but never recoil"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	world.round_sim.weapon_spread_enabled = true;
	ammo.flags = w::kAmmoFlagShotgun;
	ammo.spread_count = 1;
	ammo.kz_pieslice_bam = 0x01000000;
	world.throwables.fan_prng_state = 0x2B0749C1u;
	const int shotgun = world.round_sim.spawn(world, params);
	if (!expect(shotgun >= 0 &&
	                    world.round_sim.rounds[static_cast<size_t>(shotgun)].yaw_bam ==
	                            1614661 &&
	                    world.round_sim.rounds[static_cast<size_t>(shotgun)].pitch_bam ==
	                            -15924875 &&
	                    body->inf.recoil_pitch == (3 << 18),
	            "shotgun uses its radial fan, skips weapon ERROR, and applies recoil"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	ammo.flags = w::kAmmoFlagDesignateTarget;
	const int designator = world.round_sim.spawn(world, params);
	if (!expect(designator < 0 && world.round_sim.active_count == 0 &&
	                    world.round_sim.fired.empty() && body->inf.recoil_pitch == 0,
	            "designator returns before projectile allocation and recoil"))
		return false;

	world.round_sim.reset();
	body->inf.recoil_pitch = 0;
	ammo.flags = 0;
	body->pos[2] = 1 << 16;
	world.env.water_z = 2 << 16;
	const int submerged = world.round_sim.spawn(world, params);
	if (!expect(submerged >= 0 && body->inf.recoil_pitch == (3 << 20),
	            "submerged source uses standing-row recoil at the underwater shift"))
		return false;

	return true;
}

} // namespace

int main() {
	if (!test_retail_random_spread_vectors()) return 1;
	if (!test_spawn_spread_then_recoil()) return 1;
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	DeathAnimSource death_clips;
	ai.is_authority = true;
	ai.root_motion = &death_clips;
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
	w::AiEntity *shooter_body = ai.for_handle(hb);
	w::AiEntity *victim_body = ai.for_handle(hc);
	if (!expect(shooter_body != nullptr && victim_body != nullptr,
	            "remote player motor bodies spawned"))
		return 1;
	shooter_body->net_is_remote_peer = true;
	victim_body->net_is_remote_peer = true;
	world.add_system(&ai);

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
		defs[1].drag_fp16 = 65536;
		defs[1].min_stable_velocity = 101;
		defs[1].tumble_error_fp16 = 655;
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
		if (!expect(a->drag_fp16 == 65536 && a->min_stable_velocity == 101 &&
		                    a->tumble_error_fp16 == 655 && a->bullet_radius_fp16 == 182,
		            "exact fixed-point flight fields survive the ammo-table bake"))
			return 1;
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
		// This integration scenario isolates the existing fire/hit/death chain;
		// aerodynamic drag itself is pinned by projectile_combat_test's exact vectors.
		world.ammo.entries[1].drag = 0.0f;
		world.ammo.entries[1].drag_fp16 = 0;
	}

	ns::LoopbackChannel loop;
	ns::UdpSessionTransport udp_b(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport udp_c(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport client_b_in(ns::UdpSessionTransport::Role::Client);
	ns::NetClientView client_b_view;

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
	const Drained before_kill_b = drain_all(udp_b);
	deliver_all(before_kill_b, client_b_in);
	client_b_view.pump(client_b_in);
	const ns::ClientEntityState *remote_before = client_b_view.state().find(hc.packed);
	if (!expect(remote_before != nullptr,
	            "observer decoded the live remote victim before lethal damage"))
		return 1;
	const int32_t remote_before_x = remote_before->x;
	const int32_t remote_before_y = remote_before->y;
	const int32_t remote_before_z = remote_before->z;
	const uint8_t remote_before_anim = remote_before->anim_state_id;
	drain_all(udp_c);
	for (int shot = 0; shot < 2; ++shot) {
		dispatch_fire(roster[1], roster, world,
		              fire_body(hb.packed, 5, 0, 0, muzzle_z, 0, 0));
		for (int i = 0; i < 4; ++i) np::Server_TickUpdate(ctx, anchor);
	}
	if (!expect(world.registry.get(hc)->health == 0, "victim dead at 0 hp (clamped)")) return 1;
	const Drained after_kill_b = drain_all(udp_b);
	const Drained after_kill_c = drain_all(udp_c);
	deliver_all(after_kill_b, client_b_in);
	client_b_view.pump(client_b_in);
	const ns::ClientEntityState *remote_dead = client_b_view.state().find(hc.packed);
	if (!expect(remote_dead != nullptr,
	            "observer retained the remote victim through the death handoff"))
		return 1;
	if (!expect((remote_dead->state_flags & 0x02u) != 0u,
	            "observer decoded the remote victim's lethal/dead sample"))
		return 1;
	if (!expect(remote_before_anim < w::anim_state::kDeathFire &&
	                    remote_dead->anim_state_id >= w::anim_state::kDeathFire &&
	                    remote_dead->anim_state_id <=
	                            w::anim_state::kDeathBulletBase + 59,
	            "observer decoded the first alive-to-death-family animation edge"))
		return 1;
	if (!expect(remote_dead->x == remote_before_x &&
	                    remote_dead->y == remote_before_y &&
	                    remote_dead->z == remote_before_z,
	            "remote networked victim does not jump at lethal/death-animation handoff"))
		return 1;
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

	// --- 4. Kill the HOST player (loopback-owned): it queues for respawn and
	// releases at spawn health/position. Retail corpses remain ballistic
	// colliders (the proximity walk does not skip Flags bit 1 / dead), so move
	// the already-verified client corpse off this unrelated line-of-fire fixture.
	// Retail checks the dead-state gate after geometric impact, so a corpse is
	// not transparent to later rounds. Move this completed victim off the firing
	// lane before the separate host-player kill scenario below; the focused
	// projectile_combat test pins corpse interception itself.
	world.registry.get(hc)->position.y = 20.0f;
	for (int shot = 0; shot < 3; ++shot) {
		dispatch_fire(roster[1], roster, world,
		              fire_body(hb.packed, 5, 0, 0, muzzle_z, 0, 0));
		for (int i = 0; i < 6; ++i) np::Server_TickUpdate(ctx, anchor);
	}
	if (!expect(world.registry.get(ha)->health == 0, "host player dead")) return 1;
	if (!expect(ctx.respawn_queue.size() == 1, "host player queued for respawn")) return 1;
	{
		// Displace the corpse to prove the release snaps back [orig: the D-NET-66
		// death/respawn teleport]. The listen host's own player is MOTOR-simulated, and
		// the motor is the WRITER of the Entity/AiEntity pose pair — finish_infantry_tick
		// mirrors AiEntity.pos into Entity.position every tick. Displacing BOTH stores is
		// what makes this falsifiable: a respawn that writes only the registry Entity is
		// reverted on the next tick and the player is left standing in its own corpse's
		// spot at full health, which is indistinguishable from "I cannot respawn".
		w::Entity *host = world.registry.get(ha);
		host->position.x = 12.0f;
		w::AiEntity *host_ae = ai.for_handle(ha);
		if (!expect(host_ae != nullptr && host_ae->inf.active,
		            "the host player is motor-simulated")) return 1;
		host_ae->pos[0] = w::to_fixed(12.0);
		host_ae->inf.stance = w::InfantryState::Stance::kProne;
		for (int i = 0; i < 621; ++i) np::Server_TickUpdate(ctx, anchor);
		if (!expect(ctx.respawn_queue.empty(), "respawn released after the timer")) return 1;
		host = world.registry.get(ha);
		if (!expect(host->health == 150, "respawn restores template health")) return 1;
		if (!expect(std::fabs(host->position.x - 60.0f) < 0.01f,
		            "respawn snaps to the spawn point"))
			return 1;
		host_ae = ai.for_handle(ha);
		if (!expect(host_ae != nullptr, "the respawned host player kept its motor entity"))
			return 1;
		if (!expect(host_ae->pos[0] == w::to_fixed(60.0),
		            "respawn snaps the MOTOR store too, so the mirror cannot revert it"))
			return 1;
		if (!expect(host_ae->health == 150, "the motor health store respawns with it"))
			return 1;
		if (!expect(host_ae->inf.stance == w::InfantryState::Stance::kStand,
		            "the respawned body stands up out of the death pose"))
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
				// The deploy release re-rolls this player's TICK SEED — per connection,
				// never the session constant (a shared value would re-seed every client's
				// clock to the same tick on every deploy). The witnessed shape is
				// ((rand() & 0xFE) + 1) << 16: the low word is always zero, the seed is
				// never zero, it never exceeds 0xFF0000, and bit 16 is always set.
				// [orig: Server_SendRandomSeedToPlayer @0x5101a0 value @0x5101d4]
				const uint32_t seed = static_cast<uint32_t>(m.payload[0]) |
						(static_cast<uint32_t>(m.payload[1]) << 8) |
						(static_cast<uint32_t>(m.payload[2]) << 16) |
						(static_cast<uint32_t>(m.payload[3]) << 24);
				if (!expect(m.payload.size() == 4 && (seed & 0xFFFFu) == 0 && seed != 0 &&
				                    seed <= 0xFF0000u && (seed & 0x10000u) != 0 &&
				                    seed == conn.tick_seed,
				            "deploy 0x61 carries this connection's re-rolled tick seed"))
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
		world.ammo.entries[1].tracer_item_friendly = 1883;
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
			const w::LiveRound &round =
					world.round_sim.rounds[size_t(slot)];
			if (!expect(round.item_type_id == 1883 &&
			                    w::round_visible_item_id(round) ==
			                            (want ? 1883 : 0),
			            "TrcrID stays class-bound while only cadence tracer shots show its model"))
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
		if (!expect(
		            world.round_sim.rounds[size_t(s0)].item_type_id == 1883 &&
		                    w::round_visible_item_id(
		                            world.round_sim.rounds[size_t(s0)]) == 0,
		            "non-tracer cadence keeps the TrcrID bind but clears its visible model"))
			return 1;
		world.ammo.entries[1].flags |= 0x8000u; // forcetracer
		int s1 = world.round_sim.spawn(world, rp);
		if (!expect(s1 >= 0 && world.round_sim.rounds[size_t(s1)].tracer,
		            "FORCETRACER overrides rate 0"))
			return 1;
		if (!expect(w::round_visible_item_id(
		                    world.round_sim.rounds[size_t(s1)]) == 1883,
		            "FORCETRACER keeps the selected TrcrID model visible"))
			return 1;
		world.ammo.entries[1].flags &= ~0x8000u;

		w::RoundSim lifetime_sim;
		const int first_lifetime_slot = lifetime_sim.spawn(world, rp);
		if (!expect(first_lifetime_slot >= 0,
		            "presentation-lifetime probe spawned its first round"))
			return 1;
		const uint64_t first_generation =
				lifetime_sim.rounds[size_t(first_lifetime_slot)]
						.presentation_generation;
		lifetime_sim.rounds[size_t(first_lifetime_slot)].active = false;
		--lifetime_sim.active_count;
		const int second_lifetime_slot = lifetime_sim.spawn(world, rp);
		if (!expect(second_lifetime_slot == first_lifetime_slot &&
		                    lifetime_sim.rounds[size_t(second_lifetime_slot)]
		                                    .presentation_generation !=
		                            first_generation,
		            "same-slot reuse receives a new presentation lifetime identity"))
			return 1;

		world.ammo.entries[1].tracer_item_friendly = 0;
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
