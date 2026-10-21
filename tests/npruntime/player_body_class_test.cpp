// D-NET-348 (FOUND 3): a player body takes its bound items.def row's class-table row, by the
// row's ai_function: the class init the spawn calls (def+0x148) and the class event callback it
// copies into entity+0x1C8 (def+0x138), which the player-body update's think, the damage sites
// and the drowning call. "plyr" runs PlayerClass_InitEntity (the equipped default, the local
// lock tone) and Entity_HandleDamageAndTriggerZones; "org0" / "org1" Entity_InitOrganicAI (no
// equipped store: the memset's 0) and OrganicClass_HandleEvent (event 0 re-arms the think at
// 62, event 4 zeroes the health word, no dead return); an item class row its item body: the
// null row (an empty tag, a name no row carries) NullClass_HandleEvent (+0x2AC = 0x1000000),
// "brrl" Entity_HandleDeathOnAuthority (the authority's 1920 re-arm, the dying body's dead bit
// and 10, the dead body's explosion and S2C 0x12 removal), "ele0" its 62 re-arm.
// [orig: g_EntityClassEventCallbackTable @0x813000 (null @0x813000, org0 @0x813018, org1
//  @0x813030, plyr @0x813048, brrl @0x813060, ele0 @0x8130A8); Entity_LookupRenderCallbacks
//  @0x407DC0 - the stricmp call @0x407DD8, the row-0 default @0x407DEE; Entity_SpawnFromAnimSlotProperty
//  @0x43C390 - the init @0x43C4F0..0x43C4FE, the event copy @0x43C530..0x43C536;
//  PlayerClass_InitEntity @0x4B1060 - @0x4B10D0..0x4B10DE, @0x4B1100..0x4B1116;
//  OrganicClass_HandleEvent @0x407310 - @0x40732D, @0x407343; NullClass_HandleEvent @0x406FF0
//  (@0x406FF4); Entity_HandleDeathOnAuthority @0x407CC0 - @0x407CDE..0x407D47;
//  Server_RemoveEntityAndNotify @0x50A270 - the 0x12 @0x50A2AC; ElevatorClass_HandleEvent
//  @0x4A20D0; Entity_HandleDamageAndTriggerZones @0x407720 - the dead return @0x40772F, the
//  re-arm @0x407B5E]
// Every items.def / charattr.def row is the test's own; core-only, heap-held worlds.

#include <formats/def/def.h>
#include <runtime/inmatch/charattr_table.h>
#include <runtime/mission/item_traits.h>
#include <runtime/replication/entity_wire_bridge.h> // apply_player_intent (the pose gate)
#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/infantry.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <variant>

using namespace opennova;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// Row 0 the "Null" marker; then a plyr person (5305), an org0 person (5311), a person with no
// ai_function (5320: the null row), a brrl person (5321), an ele0 person (5322) and a nade
// person (5323: the row with no event callback).
const char kItems[] =
		"begin \"Null\"\r\n id 100000\r\n type marker\r\nend\r\n"
		"begin \"Player\"\r\n id 105305\r\n type person\r\n hp 150\r\n ai_function plyr\r\nend\r\n"
		"begin \"Soldier\"\r\n id 105311\r\n type person\r\n hp 100\r\n ai_function org0\r\nend\r\n"
		"begin \"Classless\"\r\n id 105320\r\n type person\r\n hp 100\r\nend\r\n"
		"begin \"Barrel man\"\r\n id 105321\r\n type person\r\n hp 100\r\n ai_function brrl\r\nend\r\n"
		"begin \"Lift man\"\r\n id 105322\r\n type person\r\n hp 100\r\n ai_function ele0\r\nend\r\n"
		"begin \"Grenade man\"\r\n id 105323\r\n type person\r\n hp 100\r\n ai_function nade\r\nend\r\n"
		"begin \"Satchel man\"\r\n id 105324\r\n type person\r\n hp 100\r\n ai_function schl\r\nend\r\n"
		"begin \"Claymore man\"\r\n id 105325\r\n type person\r\n hp 100\r\n ai_function clym\r\nend\r\n";

// Class 5 the plyr row, 6 the org0 row, 7 the classless row, 8 the brrl row, 9 the ele0 row
// (classes 1..4 the plyr row: the load reads CHARACTER1 on until the first missing section).
const char kCharattr[] =
		"[CHARACTER1]\r\nJUNGLE_CAMMO\t= 5305\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5305\r\n"
		"[CHARACTER2]\r\nJUNGLE_CAMMO\t= 5305\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5305\r\n"
		"[CHARACTER3]\r\nJUNGLE_CAMMO\t= 5305\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5305\r\n"
		"[CHARACTER4]\r\nJUNGLE_CAMMO\t= 5305\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5305\r\n"
		"[CHARACTER5]\r\nJUNGLE_CAMMO\t= 5305\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5305\r\n"
		"[CHARACTER6]\r\nJUNGLE_CAMMO\t= 5311\r\nDESERT_CAMMO\t= 5311\r\nARCTIC_CAMMO\t= 5311\r\n"
		"[CHARACTER7]\r\nJUNGLE_CAMMO\t= 5320\r\nDESERT_CAMMO\t= 5320\r\nARCTIC_CAMMO\t= 5320\r\n"
		"[CHARACTER8]\r\nJUNGLE_CAMMO\t= 5321\r\nDESERT_CAMMO\t= 5321\r\nARCTIC_CAMMO\t= 5321\r\n"
		"[CHARACTER9]\r\nJUNGLE_CAMMO\t= 5322\r\nDESERT_CAMMO\t= 5322\r\nARCTIC_CAMMO\t= 5322\r\n"
		"[CHARACTER10]\r\nJUNGLE_CAMMO\t= 5323\r\nDESERT_CAMMO\t= 5323\r\nARCTIC_CAMMO\t= 5323\r\n"
		"[CHARACTER11]\r\nJUNGLE_CAMMO\t= 5324\r\nDESERT_CAMMO\t= 5324\r\nARCTIC_CAMMO\t= 5324\r\n"
		"[CHARACTER12]\r\nJUNGLE_CAMMO\t= 5325\r\nDESERT_CAMMO\t= 5325\r\nARCTIC_CAMMO\t= 5325\r\n";

constexpr uint8_t kPlyr = 5, kOrg = 6, kNull = 7, kBrrl = 8, kEle0 = 9, kNade = 10, kSchl = 11,
		kClym = 12;
constexpr uint8_t kEquipped = 3;

struct Rig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<w::LocalPlayer> local;
	def::DefItemsFile items{};
	uint16_t next_net_id = 0xFF00;

	Rig() {
		CHECK(def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItems),
				sizeof(kItems) - 1, &items) == 0);
		world->registry.configure_pool(0, 8);
		world->rules.logic_authority = true;
		mission::resolve_item_traits(*world, items, {}, nullptr);
		inmatch::CharAttrTable table;
		inmatch::charattr_load(table, reinterpret_cast<const uint8_t *>(kCharattr),
				sizeof(kCharattr) - 1);
		CHECK(table.loaded);
		inmatch::install_class_attribute_flags(*world, nullptr, table);
		local = std::make_unique<w::LocalPlayer>(*world);
		world->local_player_state = local.get();
	}
	~Rig() {
		world->local_player_state = nullptr;
		def::def_free_items(&items);
	}

	// The body's bind (MissionKernel::bind_spawned_body): the traits sweep over it.
	w::Entity *bind(w::EntityHandle h) {
		if (h.valid()) mission::resolve_item_traits(*world, items, {}, nullptr, h);
		return world->registry.get(h);
	}
	// The host's own body of `player_class`, its lock tone raised ahead of the spawn.
	w::Entity *spawn_local(uint8_t player_class) {
		local->radar.lock_tone = 7;
		w::PlayerSpawn s;
		s.player_class = player_class;
		s.net_id = next_net_id++;
		s.equipped_adm_index = kEquipped;
		return bind(w::spawn_player(*world, s));
	}
	w::Entity *spawn_remote(uint8_t player_class) {
		w::PlayerSpawn s;
		s.player_class = player_class;
		s.net_id = next_net_id++;
		s.equipped_adm_index = kEquipped;
		return bind(w::spawn_remote_player(*world, s));
	}
	w::PlayerBodyClass row_class(const w::Entity &body) const {
		return w::player_item_row(*world, body).body_class;
	}
};

// The table walk: the person rows by whole name ignoring case; every other row, an empty tag
// and a miss (the row-0 default) included, an item class.
void test_the_tag_names_its_row() {
	CHECK(w::player_body_class_from_tag("plyr") == w::PlayerBodyClass::kPlayer);
	CHECK(w::player_body_class_from_tag("PLYR") == w::PlayerBodyClass::kPlayer);
	CHECK(w::player_body_class_from_tag("org0") == w::PlayerBodyClass::kOrganic);
	CHECK(w::player_body_class_from_tag("Org1") == w::PlayerBodyClass::kOrganic);
	CHECK(w::player_body_class_from_tag("brrl") == w::PlayerBodyClass::kItem);
	CHECK(w::player_body_class_from_tag("null") == w::PlayerBodyClass::kItem);
	CHECK(w::player_body_class_from_tag("") == w::PlayerBodyClass::kItem);
	CHECK(w::player_body_class_from_tag("plyr2") == w::PlayerBodyClass::kItem);
	CHECK(w::player_body_class_from_tag("chel") == w::PlayerBodyClass::kItem);
}

// The spawn's class init by the bound row: the player class stores the equipped default and
// resets the local body's lock tone; the organic row and the item rows with no init (null,
// brrl, ele0) store neither (the equipped byte the plyr serializer echoes stays the memset's 0).
void test_the_spawn_runs_its_rows_init() {
	struct Case {
		uint8_t player_class;
		w::PlayerBodyClass cls;
		uint8_t equipped;
		int32_t lock_tone;
	};
	for (const Case c : {Case{kPlyr, w::PlayerBodyClass::kPlayer, kEquipped, 0},
				Case{kOrg, w::PlayerBodyClass::kOrganic, 0, 7},
				Case{kNull, w::PlayerBodyClass::kItem, 0, 7},
				Case{kBrrl, w::PlayerBodyClass::kItem, 0, 7},
				Case{kEle0, w::PlayerBodyClass::kItem, 0, 7}}) {
		Rig r;
		w::Entity *body = r.spawn_local(c.player_class);
		CHECK(body != nullptr);
		if (body == nullptr) continue;
		CHECK(r.row_class(*body) == c.cls);
		CHECK(body->equipped_adm_index == c.equipped);
		CHECK(r.local->radar.lock_tone == c.lock_tone);
		// Every class's body is the local player (the mission start's bind).
		CHECK(r.world->cached.local_player == body->handle);
		// A remote body's equipped byte follows the row the same way.
		const w::Entity *remote = r.spawn_remote(c.player_class);
		CHECK(remote != nullptr && remote->equipped_adm_index == c.equipped);
	}
}

// The think (event 0) by the bound row: plyr re-arms at 64 (and a dead body returns), org at
// 62, the null row at 0x1000000, the barrel row at 1920 for a live body on the authority, the
// ele0 row at 62.
void test_the_think_runs_its_rows_callback() {
	Rig r;
	w::Entity *plyr = r.spawn_remote(kPlyr);
	w::Entity *org = r.spawn_remote(kOrg);
	w::Entity *null_row = r.spawn_remote(kNull);
	w::Entity *brrl = r.spawn_remote(kBrrl);
	w::Entity *ele0 = r.spawn_remote(kEle0);
	CHECK(plyr != nullptr && org != nullptr && null_row != nullptr && brrl != nullptr &&
			ele0 != nullptr);
	if (plyr == nullptr || org == nullptr || null_row == nullptr || brrl == nullptr ||
			ele0 == nullptr)
		return;
	w::player_body_row_event(*r.world, *plyr, 0);
	CHECK(plyr->spawn_phase == 64);
	w::player_body_row_event(*r.world, *org, 0);
	CHECK(org->spawn_phase == 62);
	w::player_body_row_event(*r.world, *null_row, 0);
	CHECK(null_row->spawn_phase == 0x1000000);
	w::player_body_row_event(*r.world, *brrl, 0);
	CHECK(brrl->spawn_phase == 1920);
	ele0->spawn_phase = -3;
	w::player_body_row_event(*r.world, *ele0, 0);
	CHECK(ele0->spawn_phase == 62);
	// A dead plyr body returns at the head; a dead org body still re-arms (no dead return).
	plyr->spawn_phase = -5;
	plyr->engine_flags |= w::kEntityFlagDead;
	w::player_body_row_event(*r.world, *plyr, 0);
	CHECK(plyr->spawn_phase == -5);
	org->spawn_phase = -5;
	org->engine_flags |= w::kEntityFlagDead;
	w::player_body_row_event(*r.world, *org, 0);
	CHECK(org->spawn_phase == 62);
}

// The barrel row's authority think on a body at or below 0 health: the dead bit and 10; then,
// dead, the explosion and the removal (in a session the S2C 0x12 a joiner sees); off the
// authority nothing.
void test_the_barrel_rows_dying_body() {
	Rig r;
	w::Entity *brrl = r.spawn_remote(kBrrl);
	CHECK(brrl != nullptr);
	if (brrl == nullptr) return;
	brrl->health = 0;
	w::player_body_row_event(*r.world, *brrl, 0);
	CHECK((brrl->engine_flags & w::kEntityFlagDead) != 0 && brrl->spawn_phase == 10);
	r.world->rules.mp_session = true;
	r.world->out.entity_events.clear();
	const w::EntityHandle gone = brrl->handle;
	w::player_body_row_event(*r.world, *brrl, 0);
	CHECK(r.world->registry.get(gone) == nullptr);
	bool removal = false;
	for (const auto &ev : r.world->out.entity_events)
		if (const auto *rm = std::get_if<w::EntityRemoveEvent>(&ev))
			removal = removal || rm->handle == gone.packed;
	CHECK(removal);

	Rig client;
	client.world->rules.logic_authority = false;
	w::Entity *other = client.spawn_remote(kBrrl);
	CHECK(other != nullptr);
	if (other == nullptr) return;
	other->spawn_phase = -1;
	w::player_body_row_event(*client.world, *other, 0);
	CHECK(other->spawn_phase == -1);
}

// A damage site's event 4 (the class callback a kill fires) by the row: the organic class
// zeroes the health word; the plyr class keeps it (its event-4 arm is the authority's seat
// leave, then the health tail).
void test_a_kill_event_runs_its_rows_callback() {
	Rig r;
	w::Entity *plyr = r.spawn_remote(kPlyr);
	w::Entity *org = r.spawn_remote(kOrg);
	CHECK(plyr != nullptr && org != nullptr);
	if (plyr == nullptr || org == nullptr) return;
	const int32_t plyr_health = plyr->health;
	w::hit_record_class_event(*r.world, *plyr, 4);
	CHECK(plyr->health == plyr_health);
	w::hit_record_class_event(*r.world, *org, 4);
	CHECK(org->health == 0);
}

// The knife's kill zone calls the victim's callback with event 3 (no null test): a player body
// runs its bound row's (D-NET-348): a brrl row's body re-arms at 1920 with no plyr 64 and no
// waypoint tail; a plyr row's body re-arms at 64.
// [orig: Entity_ApplyVehicleCollisionDamage - the event word @0x4E673A, the call @0x4E6752;
//  Entity_HandleDeathOnAuthority @0x407CC0 (1920 @0x407D47); Entity_HandleDamageAndTriggerZones
//  @0x407B5E, the tail @0x407B64..0x407C6B]
void test_a_knife_runs_its_rows_callback() {
	for (const uint8_t player_class : {kBrrl, kPlyr}) {
		Rig r;
		w::AmmoTableEntry null_e;
		null_e.name = "AT_NULL";
		null_e.valid = true;
		r.world->tables.ammo.entries.push_back(null_e);
		w::AmmoTableEntry cut;
		cut.name = "KNIFE";
		cut.valid = true;
		cut.kztype = w::ammo_kz::kKnife;
		cut.kz_damage = 20;
		cut.kz_maxradius = 1.5f;
		r.world->tables.ammo.entries.push_back(cut); // index 1
		w::Entity source;
		source.item_type_index = 1;
		source.kind = w::EntityKind::Organic;
		source.has_item_def = true;
		source.item_type = 3;
		source.health = 100;
		source.team = 2;
		const w::EntityHandle attacker = r.world->registry.spawn(0, source);
		w::Entity *body = r.spawn_remote(player_class);
		CHECK(body != nullptr);
		if (body == nullptr) continue;
		const w::EntityHandle victim = body->handle;
		body->team = 1;
		body->net_id = 7;
		body->position = w::Vec3{1.0f, 0.0f, 0.0f};
		body->spawn_phase = 5;
		const int32_t health = body->health;
		w::AiEntity *ai = r.world->ai.for_handle(victim);
		CHECK(ai != nullptr);
		if (ai == nullptr) continue;
		ai->self->position.x.q16 = w::to_fixed(1.0f);
		ai->slot.f[37] = 3;
		r.world->ai.nav.channels.resize(4);
		r.world->ai.nav.channels[3].count = 1;
		r.world->ai.nav.channels[3].entries[0] = 0;
		r.world->ai.nav.nodes.resize(1);
		r.world->ai.nav.nodes[0].f[0] = 0x10000;
		r.world->ai.nav.nodes[0].f[1] = ai->self->position.x.q16;
		r.world->ai.nav.nodes[0].f[2] = ai->self->position.y.q16;
		w::ExplosionEntry e;
		e.pos = w::Vec3{};
		e.type = w::ammo_kz::kKnife;
		e.ammo_index = 1;
		e.owner = attacker;
		r.world->explosions.queue_explosion(*r.world, e);
		r.world->explosions.process(*r.world, nullptr, nullptr, -1.0e9f, r.world->out.destruction);
		const w::Entity *hit = r.world->registry.get(victim);
		CHECK(hit != nullptr && hit->health == health - 20);
		if (hit == nullptr) continue;
		const bool plyr = player_class == kPlyr;
		CHECK(hit->spawn_phase == (plyr ? 64 : 1920));
		CHECK(r.world->script.relations.single_visited(7, 3, 0) == plyr);
	}
}

// A round's hit calls the victim's callback with event 1 after its null test: a brrl row's player
// body runs the barrel's (the 1920 re-arm), no person legs, no plyr 64, no waypoint tail; the
// nade row's (no callback) runs nothing.
// [orig: Projectile_ProcessDamageOnTarget - the event word @0x4E81FF, the null test @0x4E8205,
//  the call @0x4E820E; Entity_HandleDeathOnAuthority @0x407CC0 (1920 @0x407D47)]
void test_a_round_hit_runs_its_rows_callback() {
	for (const uint8_t player_class : {kBrrl, kNade}) {
		Rig r;
		r.world->ai.is_authority = true;
		w::CollisionWorld collision;
		w::Entity s;
		s.kind = w::EntityKind::Organic;
		s.item_type_index = 3;
		s.item_type = 3;
		s.team = 2;
		s.health = 100;
		const w::EntityHandle shooter = r.world->registry.spawn(0, s);
		w::PlayerSpawn spawn;
		spawn.position = {5.0f, 0.0f, 0.0f};
		spawn.net_id = 0xFFF1;
		spawn.player_class = player_class;
		spawn.heading = w::placed_heading_bam(90);
		w::Entity *body = r.bind(w::spawn_remote_player(*r.world, spawn));
		CHECK(body != nullptr);
		if (body == nullptr) continue;
		const w::EntityHandle victim = body->handle;
		body->bound_radius = 1.0f;
		body->team = 1;
		body->net_id = 7;
		body->spawn_phase = 5;
		body->health = 5000; // the round leaves it alive
		w::AiEntity *ai = r.world->ai.for_handle(victim);
		CHECK(ai != nullptr);
		if (ai == nullptr) continue;
		ai->net_is_remote_peer = true;
		ai->slot.f[37] = 3;
		r.world->ai.nav.channels.resize(4);
		r.world->ai.nav.channels[3].count = 1;
		r.world->ai.nav.channels[3].entries[0] = 0;
		r.world->ai.nav.nodes.resize(1);
		r.world->ai.nav.nodes[0].f[0] = 0x10000;
		r.world->ai.nav.nodes[0].f[1] = ai->self->position.x.q16;
		r.world->ai.nav.nodes[0].f[2] = ai->self->position.y.q16;
		w::AmmoTableEntry ammo;
		ammo.name = "ZONE";
		ammo.valid = true;
		ammo.velocity = 620;
		ammo.max_age_ticks = 20;
		ammo.weight_in_grains = 875;
		r.world->tables.ammo.entries.push_back(ammo);
		w::CollisionModel model;
		model.sections.resize(14);
		for (w::CollisionSection &section : model.sections) {
			section.authored_bounds = true;
			section.min_x = section.min_y = section.min_z = -0x10000;
			section.max_x = section.max_y = section.max_z = 0x10000;
			section.radius = 0x10000;
		}
		collision.assign_entity(victim, collision.add_model(std::move(model)));
		r.world->collision = &collision;
		std::vector<w::CollisionMatrix> matrices;
		for (int section = 0; section < 14; ++section) {
			const int32_t center[3] = {5 * 65536, section == 13 ? 0 : (20 + section) * 65536, 58982};
			matrices.push_back(w::collision_matrix_from_heading(0, center));
		}
		CHECK(collision.publish_entity_section_matrices(victim, std::move(matrices)));
		collision.build_tick_tables(*r.world);
		const int32_t death_anim = body->death_anim_state;
		w::RoundSpawnParams params;
		params.owner = shooter;
		params.shooter_handle = shooter.packed;
		params.origin = {0.0f, 0.0f, 0.9f};
		params.ammo_index = 0;
		CHECK(r.world->round_sim.spawn(*r.world, params) >= 0);
		r.world->round_sim.tick(*r.world, nullptr);
		const w::Entity *hit = r.world->registry.get(victim);
		CHECK(hit != nullptr);
		if (hit == nullptr) continue;
		CHECK(r.world->round_sim.hit_record.target == victim); // the round struck the body
		CHECK(hit->spawn_phase == (player_class == kBrrl ? 1920 : 5));
		CHECK(hit->death_anim_state == death_anim); // no person legs
		CHECK(!r.world->script.relations.single_visited(7, 3, 0));
		r.world->collision = nullptr;
	}
}

// One round fired into a remote player body of `player_class` (the rig of the case above): its
// health before the round, whether it is already dead, whether it is the local player's.
struct RoundHitOutcome {
	bool struck = false;
	int32_t death_anim_state = 0;
	int32_t motor_anim_state = -1;
	int motor_anim_pending = -1;
	int32_t shake = 0;
};
RoundHitOutcome fire_one_round(uint8_t player_class, int32_t health, bool dead, bool local) {
	RoundHitOutcome out;
	Rig r;
	r.world->ai.is_authority = true;
	w::CollisionWorld collision;
	w::Entity s;
	s.kind = w::EntityKind::Organic;
	s.item_type_index = 3;
	s.item_type = 3;
	s.team = 2;
	s.health = 100;
	const w::EntityHandle shooter = r.world->registry.spawn(0, s);
	w::PlayerSpawn spawn;
	spawn.position = {5.0f, 0.0f, 0.0f};
	spawn.net_id = 0xFFF1;
	spawn.player_class = player_class;
	spawn.heading = w::placed_heading_bam(90);
	w::Entity *body = r.bind(w::spawn_remote_player(*r.world, spawn));
	CHECK(body != nullptr);
	if (body == nullptr) return out;
	const w::EntityHandle victim = body->handle;
	body->bound_radius = 1.0f;
	body->team = 1;
	body->health = health;
	body->death_anim_state = 0x1234; // a clip no select produces
	if (dead) body->flags |= w::kEntityFlagDead;
	if (local) r.world->cached.local_player = victim;
	w::AmmoTableEntry ammo;
	ammo.name = "ZONE";
	ammo.valid = true;
	ammo.velocity = 620;
	ammo.max_age_ticks = 20;
	ammo.weight_in_grains = 875;
	r.world->tables.ammo.entries.push_back(ammo);
	w::CollisionModel model;
	model.sections.resize(14);
	for (w::CollisionSection &section : model.sections) {
		section.authored_bounds = true;
		section.min_x = section.min_y = section.min_z = -0x10000;
		section.max_x = section.max_y = section.max_z = 0x10000;
		section.radius = 0x10000;
	}
	collision.assign_entity(victim, collision.add_model(std::move(model)));
	r.world->collision = &collision;
	std::vector<w::CollisionMatrix> matrices;
	for (int section = 0; section < 14; ++section) {
		const int32_t center[3] = {5 * 65536, section == 13 ? 0 : (20 + section) * 65536, 58982};
		matrices.push_back(w::collision_matrix_from_heading(0, center));
	}
	CHECK(collision.publish_entity_section_matrices(victim, std::move(matrices)));
	collision.build_tick_tables(*r.world);
	w::RoundSpawnParams params;
	params.owner = shooter;
	params.shooter_handle = shooter.packed;
	params.origin = {0.0f, 0.0f, 0.9f};
	params.ammo_index = 0;
	CHECK(r.world->round_sim.spawn(*r.world, params) >= 0);
	r.world->round_sim.tick(*r.world, nullptr);
	r.world->collision = nullptr;
	out.struck = r.world->round_sim.hit_record.target == victim;
	out.shake = r.local->view.shake.counter;
	const w::Entity *hit = r.world->registry.get(victim);
	if (hit == nullptr) return out;
	out.death_anim_state = hit->death_anim_state;
	if (const w::AiEntity *motor = r.world->ai.for_handle(victim)) {
		out.motor_anim_state = motor->inf.anim_state;
		out.motor_anim_pending = motor->inf.anim_pending;
	}
	return out;
}

// The round hit's plyr-row arm runs the plyr callback's health tail: a body the round leaves at
// Health <= 0 requests its staged death clip on its primary channel, the deferred one cleared;
// an org row's clip select runs on a dead body too (its callback has no dead return); and the
// local player's damage feedback reads the dead bit after the callback, so a brrl row's body the
// round kills (the barrel's dying arm sets the bit) arms none where a plyr row's does.
// [orig: Entity_HandleDamageAndTriggerZones @0x407720 - the dead return @0x40772F, the health
//  tail @0x407ACD..0x407B33 (+0x2BC @0x407AFB, +0x2B8 @0x407B01); OrganicClass_HandleEvent
//  @0x407310 - the select @0x407483; Projectile_ProcessDamageOnTarget @0x4E7FB0 - the callback
//  @0x4E820E, `test byte [esi+24h], 2` @0x4E8213, Player_OnDamageReceived @0x4E822B;
//  Entity_HandleDeathOnAuthority @0x407CC0 - `or [esi+24h], 2` @0x407D34]
void test_a_round_hit_runs_the_callbacks_tails() {
	const RoundHitOutcome lethal = fire_one_round(kPlyr, 10, /*dead=*/false, /*local=*/true);
	CHECK(lethal.struck);
	CHECK(lethal.death_anim_state != 0x1234);
	CHECK(lethal.motor_anim_state == lethal.death_anim_state);
	CHECK(lethal.motor_anim_pending == 0);
	CHECK(lethal.shake > 0); // the plyr callback leaves the bit clear: the feedback arms
	const RoundHitOutcome alive = fire_one_round(kPlyr, 5000, false, false);
	CHECK(alive.struck);
	CHECK(alive.motor_anim_state != alive.death_anim_state); // Health > 0: no tail
	const RoundHitOutcome corpse = fire_one_round(kOrg, 100, /*dead=*/true, false);
	if (corpse.struck) CHECK(corpse.death_anim_state != 0x1234); // the org select, dead or not
	const RoundHitOutcome plyr_corpse = fire_one_round(kPlyr, 100, /*dead=*/true, false);
	if (plyr_corpse.struck) CHECK(plyr_corpse.death_anim_state == 0x1234); // the dead return
	const RoundHitOutcome barrel = fire_one_round(kBrrl, 10, false, /*local=*/true);
	CHECK(barrel.struck);
	CHECK(barrel.shake == 0); // the dying arm's bit, read after the callback
}

// Script SSN 10000 names the local body only when its row is the plyr row: the plyr class init
// alone stamps the DcbId with 10000 + the slot byte; an org or item row's body keeps the spawn
// memset's 0, so the SSN names nothing.
// [orig: PlayerClass_InitEntity @0x4B1060 - @0x4B1149..0x4B1173; Entity_InitOrganicAI @0x4BFCC0;
//  Entity_SpawnFromAnimSlotProperty @0x43C390 - the memset @0x43C3FE; EntityPool_FindByNetId
//  @0x4F0A20]
void test_the_player_ssn_follows_the_plyr_class_init() {
	for (const uint8_t player_class : {kPlyr, kOrg, kBrrl}) {
		Rig r;
		const w::Entity *body = r.spawn_local(player_class);
		CHECK(body != nullptr);
		if (body == nullptr) continue;
		const w::EntityHandle h = body->handle;
		const w::EntityCommands commands(*r.world);
		const w::EntityHandle named = commands.resolve_ssn(10000); // mission::kPlayerSsn
		if (player_class == kPlyr)
			CHECK(named == h);
		else
			CHECK(!named.valid());
	}
}

// The barrel row's dying arm stores the dead bit into the Flags word every player path reads, so
// the uplink's pose gate drops the body's pose from that tick.
// [orig: Entity_HandleDeathOnAuthority @0x407CC0 - `or [esi+24h], 2` @0x407D34; the uplink's
//  `test byte [edi+24h], 2` @0x4C2000]
void test_the_barrel_rows_dying_flag_reaches_the_uplink_gate() {
	Rig r;
	w::Entity *brrl = r.spawn_remote(kBrrl);
	CHECK(brrl != nullptr);
	if (brrl == nullptr) return;
	brrl->health = 0;
	CHECK((brrl->flags & w::kEntityFlagDead) == 0);
	replication::PlayerIntent intent;
	intent.entity_handle = brrl->handle.packed;
	CHECK(replication::apply_player_intent(*r.world, intent, /*slot_in_game=*/true));
	w::player_body_row_event(*r.world, *brrl, 0);
	CHECK((brrl->flags & w::kEntityFlagDead) != 0);
	CHECK((brrl->engine_flags & w::kEntityFlagDead) != 0);
	CHECK(!replication::apply_player_intent(*r.world, intent, /*slot_in_game=*/true));
}

// The nade row's event word is 0: the think calls nothing (the counter runs down on).
// [orig: the nade row @0x813138; Entity_UpdateInfantryPlayerBody @0x4B4BD2..0x4B4BDA]
void test_the_nade_row_has_no_callback() {
	Rig r;
	w::Entity *nade = r.spawn_remote(kNade);
	CHECK(nade != nullptr);
	if (nade == nullptr) return;
	CHECK(r.row_class(*nade) == w::PlayerBodyClass::kItem);
	nade->spawn_phase = -3;
	w::player_body_row_event(*r.world, *nade, 0);
	CHECK(nade->spawn_phase == -3);
}

// A satchel or claymore row's body runs the device's think: on the authority a live body
// (the claymore's cone over ammo 0, the body's +0x26C word, finds nobody) keeps standing; a
// body at 0 health spawns the boom rounds at its position (the satchel's satchelboom, the
// claymore's claymoreshrapnel and claymorekillzone), sends its state packet (section 0) in a
// session and is destroyed. Off the authority the think does nothing; the kill's event 4
// fires the body with no packet.
// [orig: Entity_SatchelThink @0x443670 (@0x443771, @0x4437E2, @0x443895, @0x44389E, event 4
//  @0x443698); Entity_ClaymoreThink @0x4438C0 (@0x443A3F, @0x443ACC / @0x443B32, @0x443B85,
//  @0x443B8E, event 4 @0x4438FD)]
void test_a_device_rows_body_runs_the_device_think() {
	for (const uint8_t player_class : {kSchl, kClym}) {
		const bool claymore = player_class == kClym;
		for (const bool authority : {true, false}) {
			Rig r;
			r.world->rules.logic_authority = authority;
			r.world->rules.mp_session = true;
			for (const char *name : {"AT_NULL", "satchelboom", "claymoreshrapnel",
					"claymorekillzone"}) {
				w::AmmoTableEntry ammo;
				ammo.name = name;
				ammo.valid = true;
				ammo.velocity = 100;
				ammo.max_age_ticks = 20;
				r.world->tables.ammo.entries.push_back(ammo);
			}
			w::Entity *body = r.spawn_remote(player_class);
			CHECK(body != nullptr);
			if (body == nullptr) continue;
			CHECK(w::player_item_row(*r.world, *body).device_think ==
					(claymore ? w::ThrowClass::kClaymore : w::ThrowClass::kSatchel));
			const w::EntityHandle h = body->handle;
			w::player_body_row_event(*r.world, *body, 0);
			CHECK(r.world->registry.get(h) != nullptr); // alive: it stands
			CHECK(r.world->round_sim.active_count == 0);
			body->health = 0;
			r.world->out.entity_events.clear();
			w::player_body_row_event(*r.world, *body, 0);
			if (!authority) {
				CHECK(r.world->registry.get(h) != nullptr); // only the kill's event 4
				w::player_body_row_event(*r.world, *body, 4);
			}
			CHECK(r.world->registry.get(h) == nullptr);
			CHECK(r.world->round_sim.active_count == (claymore ? 2 : 1));
			bool packet = false;
			for (const auto &ev : r.world->out.entity_events)
				if (const auto *state = std::get_if<w::ItemStateEvent>(&ev))
					packet = packet || (state->handle == h.packed && state->section == 0);
			CHECK(packet == authority);
		}
	}
}

} // namespace

int main() {
	test_the_tag_names_its_row();
	test_the_spawn_runs_its_rows_init();
	test_the_think_runs_its_rows_callback();
	test_the_barrel_rows_dying_body();
	test_a_kill_event_runs_its_rows_callback();
	test_a_knife_runs_its_rows_callback();
	test_a_round_hit_runs_its_rows_callback();
	test_a_round_hit_runs_the_callbacks_tails();
	test_the_player_ssn_follows_the_plyr_class_init();
	test_the_barrel_rows_dying_flag_reaches_the_uplink_gate();
	test_the_nade_row_has_no_callback();
	test_a_device_rows_body_runs_the_device_think();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_player_body_class\n");
	return 0;
}
