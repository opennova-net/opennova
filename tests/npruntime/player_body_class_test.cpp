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
//  @0x813030, plyr @0x813048, brrl @0x813060, ele0 @0x8130A8); EntityDef_LookupClassCallbacks
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
#include <runtime/hud/tip_system.h> // kTipEventDetach (the local seat leave)
#include <runtime/inmatch/charattr_table.h>
#include <runtime/mission/item_traits.h>
#include <runtime/replication/entity_wire_bridge.h> // apply_player_intent (the pose gate)
#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/entity_spawn.h> // player_class_init_animation
#include <runtime/world/round_sim.h>
#include <runtime/world/infantry.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using namespace opennova;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// Row 0 the "Null" marker; then a plyr person (5305, move function org2), an org0 person (5311,
// move function org1), a person with no ai_function (5320: the null row; no move function, the
// null physics row too), a brrl person (5321), an ele0 person (5322), a nade
// person (5323: the row with no event callback), a schl person (5324, the one row with a
// nonzero `score` word), a clym person (5325), a plyr person moved by the genx row (5326) and an
// arty person moved by the arty row (5327; its tag in capitals, the lookup ignoring case).
const char kItems[] =
		"begin \"Null\"\r\n id 100000\r\n type marker\r\nend\r\n"
		"begin \"Player\"\r\n id 105305\r\n type person\r\n hp 150\r\n ai_function plyr\r\n"
		" move_function org2\r\nend\r\n"
		"begin \"Soldier\"\r\n id 105311\r\n type person\r\n hp 100\r\n ai_function org0\r\n"
		" move_function org1\r\nend\r\n"
		"begin \"Classless\"\r\n id 105320\r\n type person\r\n hp 100\r\nend\r\n"
		"begin \"Barrel man\"\r\n id 105321\r\n type person\r\n hp 100\r\n ai_function brrl\r\nend\r\n"
		"begin \"Lift man\"\r\n id 105322\r\n type person\r\n hp 100\r\n ai_function ele0\r\nend\r\n"
		"begin \"Grenade man\"\r\n id 105323\r\n type person\r\n hp 100\r\n ai_function nade\r\nend\r\n"
		"begin \"Satchel man\"\r\n id 105324\r\n type person\r\n hp 100\r\n score 25\r\n"
		" ai_function schl\r\nend\r\n"
		"begin \"Claymore man\"\r\n id 105325\r\n type person\r\n hp 100\r\n ai_function clym\r\nend\r\n"
		"begin \"Rider man\"\r\n id 105326\r\n type person\r\n hp 100\r\n ai_function plyr\r\n"
		" move_function genx\r\nend\r\n"
		"begin \"Barrage man\"\r\n id 105327\r\n type person\r\n hp 100\r\n ai_function ARTY\r\n"
		" move_function arty\r\nend\r\n";

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
		"[CHARACTER12]\r\nJUNGLE_CAMMO\t= 5325\r\nDESERT_CAMMO\t= 5325\r\nARCTIC_CAMMO\t= 5325\r\n"
		"[CHARACTER13]\r\nJUNGLE_CAMMO\t= 5326\r\nDESERT_CAMMO\t= 5326\r\nARCTIC_CAMMO\t= 5326\r\n"
		"[CHARACTER14]\r\nJUNGLE_CAMMO\t= 5327\r\nDESERT_CAMMO\t= 5327\r\nARCTIC_CAMMO\t= 5327\r\n";

constexpr uint8_t kPlyr = 5, kOrg = 6, kNull = 7, kBrrl = 8, kEle0 = 9, kNade = 10, kSchl = 11,
		kClym = 12, kGenx = 13, kArty = 14;
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
// [orig: Entity_ApplyKnifeKillZoneDamage - the event word @0x4E673A, the call @0x4E6752;
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
	int32_t health_after = 0;
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
	if (w::AiEntity *motor = r.world->ai.for_handle(victim)) motor->inf.anim_state = 0x777;
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
	out.health_after = hit->health;
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
	// The round's damage, read off a body it leaves alive (Health > 0: no tail, the motor's
	// request stands); each lethal case below starts at exactly that health.
	const RoundHitOutcome alive = fire_one_round(kPlyr, 5000, /*dead=*/false, /*local=*/true);
	CHECK(alive.struck);
	CHECK(alive.motor_anim_state == 0x777);
	CHECK(alive.shake > 0 || 5000 - alive.health_after <= 5);
	const int32_t dealt = 5000 - alive.health_after;
	CHECK(dealt > 5);
	const RoundHitOutcome lethal = fire_one_round(kPlyr, dealt, false, /*local=*/true);
	CHECK(lethal.struck);
	CHECK(lethal.health_after <= 0);
	CHECK(lethal.death_anim_state != 0x1234);
	CHECK(lethal.motor_anim_state == lethal.death_anim_state);
	CHECK(lethal.motor_anim_pending == 0);
	CHECK(lethal.shake > 0); // the plyr callback leaves the bit clear: the feedback arms
	const RoundHitOutcome corpse = fire_one_round(kOrg, 100, /*dead=*/true, false);
	CHECK(corpse.struck && corpse.death_anim_state != 0x1234); // the org select, dead or not
	const RoundHitOutcome plyr_corpse = fire_one_round(kPlyr, 100, /*dead=*/true, false);
	CHECK(plyr_corpse.struck && plyr_corpse.death_anim_state == 0x1234); // the dead return
	const RoundHitOutcome barrel = fire_one_round(kBrrl, dealt, false, /*local=*/true);
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


// The rigs' shooter: a team-2 organic person, no player.
w::EntityHandle spawn_shooter(Rig &r) {
	w::Entity s;
	s.kind = w::EntityKind::Organic;
	s.item_type_index = 3;
	s.item_type = 3;
	s.team = 2;
	s.health = 100;
	return r.world->registry.spawn(0, s);
}

// One round's hit straight into a body at the round sim's damage site: the ZONE ammo at 10
// units a tick, a person hit of far more than the health any case below gives the body, so the
// damage is that health (the clamp to Health) and the hit is lethal.
void hit_body(Rig &r, w::EntityHandle shooter, w::EntityHandle victim) {
	if (r.world->tables.ammo.entries.empty()) {
		w::AmmoTableEntry ammo;
		ammo.name = "ZONE";
		ammo.valid = true;
		ammo.velocity = 620;
		ammo.max_age_ticks = 20;
		ammo.weight_in_grains = 875;
		r.world->tables.ammo.entries.push_back(ammo);
	}
	w::LiveRound &round = r.world->round_sim.rounds[0];
	round = w::LiveRound{};
	round.active = true;
	round.owner = shooter;
	round.shooter_handle = shooter.packed;
	round.ammo_index = 0;
	round.vel = {10.0f, 0.0f, 0.0f};
	w::ProjectileHit hit;
	hit.hit_class = w::ProjectileHitClass::Person;
	hit.geometry_entity = victim;
	hit.section_index = 1;
	hit.bone_index = 1;
	hit.hit_zone = 1;
	w::FixedVec3 velocity{10 * 65536, 0, 0};
	r.world->round_sim.process_damage_hit(*r.world, round, hit, velocity);
}

// The projectile site's lethal gate runs ahead of the subtraction and the hit record
// (D-NET-NEW-L4-16): a scored body's kill tallies there, while the body exists, so a satchel
// row's body, which its event 1 then destroys, still scores once (by the local player: a team-1
// person, the blue count) and leaves no record for the death routing; an org row's body takes
// its event 4 (Health 0) before the subtraction and so ends at Health == -damage; a seated plyr
// row's body leaves its seat at event 4: on a dedicated host by the detach, on a listen host
// its own body by the one local detach (one tip), while a remote player's body keeps its seat
// (its own 0x27 is the joiner's).
// [orig: Projectile_ProcessDamageOnTarget @0x4E7FB0 - the gate @0x4E80F7..0x4E8119, the dead
//  test @0x4E811F, the Score_ProcessKillEvent call @0x4E8133, Flags & 0x100 @0x4E8151..0x4E8159,
//  the callback (victim, 4, 0) @0x4E819C..0x4E81A1, Health -= damage @0x4E81B3, the record
//  @0x4E81BA, the event-1 callback @0x4E820E; Score_ProcessKillEvent @0x4FD400 - the score word
//  @0x4FD422, `cmp edi, g_LocalPlayerEntity` @0x4FD449, Score_TallyKillByLocalPlayer @0x4FD160;
//  OrganicClass_HandleEvent @0x407310 (the event-4 arm @0x407343); Entity_HandleDamageAndTriggerZones
//  @0x407720 (the event-4 leave @0x407A8D..0x407ABD); Entity_SendDetachPacket @0x435510 (the
//  local-player gate @0x43552A); Entity_SatchelThink @0x443670]
void test_the_projectile_kill_runs_ahead_of_the_subtraction() {
	{
		Rig r;
		r.world->ai.is_authority = true;
		const w::EntityHandle shooter = spawn_shooter(r);
		r.world->cached.local_player = shooter;
		w::Entity *body = r.spawn_remote(kSchl);
		CHECK(body != nullptr);
		if (body == nullptr) return;
		const w::EntityHandle victim = body->handle;
		CHECK(body->item_score == 25 && body->item_type == 3);
		body->team = 1;
		body->health = 7;
		hit_body(r, shooter, victim);
		CHECK(r.world->registry.get(victim) == nullptr); // its event 1 destroyed it
		CHECK(r.world->kill_stats.bluekills_by_player == 1);
		CHECK(r.world->round_sim.deaths.empty());
	}
	{
		Rig r;
		r.world->ai.is_authority = true;
		const w::EntityHandle shooter = spawn_shooter(r);
		w::Entity *body = r.spawn_remote(kOrg);
		CHECK(body != nullptr);
		if (body == nullptr) return;
		const w::EntityHandle victim = body->handle;
		body->team = 1;
		body->health = 7;
		hit_body(r, shooter, victim);
		const w::Entity *hit = r.world->registry.get(victim);
		CHECK(hit != nullptr && hit->health == -7);
		CHECK(r.world->round_sim.deaths.size() == 1);
	}
	for (const bool listen : {false, true}) {
		Rig r;
		r.world->registry.configure_pool(1, 4);
		r.world->ai.is_authority = true;
		const w::EntityHandle shooter = spawn_shooter(r);
		const w::Entity *own_body = r.spawn_local(kPlyr);
		const w::Entity *remote_body = r.spawn_remote(kPlyr);
		CHECK(own_body != nullptr && remote_body != nullptr);
		if (own_body == nullptr || remote_body == nullptr) return;
		const w::EntityHandle own = own_body->handle;
		const w::EntityHandle remote = remote_body->handle;
		CHECK(r.world->cached.local_player == own);
		// The session host: a dedicated one is no session peer, a listen host is.
		r.world->rules.mp_session = true;
		r.world->rules.projectile_authority = true;
		r.world->rules.mp_session_peer = listen;
		for (const w::EntityHandle h : {own, remote}) {
			w::Entity *e = r.world->registry.get(h);
			e->team = 1;
			e->health = 7;
			e->mounted = true;
			e->flags |= w::kEntityFlagMounted;
			e->mount_target = w::EntityHandle::make(1, 2);
			e->mount_type = w::SeatType::Passenger;
		}
		r.world->out.tip_events.clear();
		hit_body(r, shooter, own);
		hit_body(r, shooter, remote);
		CHECK(!r.world->registry.get(own)->mounted);
		CHECK(r.world->registry.get(remote)->mounted == listen);
		CHECK(std::count(r.world->out.tip_events.begin(), r.world->out.tip_events.end(),
				static_cast<uint8_t>(hud::kTipEventDetach)) == 1);
		CHECK(r.world->out.detach_requests.empty());
	}
}

// A motion source holding the reset and idle clips: each update moves a channel one tick on and
// offers a vertical root delta, which the plyr warm-up drops.
struct Clips final : w::IRootMotionSource {
	std::vector<int> calls;
	bool has_clip(int, int state) const override {
		return state == w::anim_state::kReset || state == w::anim_state::kIdle;
	}
	int32_t clip_length_ticks(int, int, int) const override { return 10000; }
	bool advance(int, int state, int32_t &phase, w::RootMotionFrame &out) override {
		if (!has_clip(0, state)) return false;
		calls.push_back(state);
		++phase;
		out = {};
		out.dz = 0x8000;
		out.capsule_bottom = 0x10000;
		out.capsule_top = 0x20000;
		return true;
	}
};

// The plyr class init on the authority's spawn, its body registered: the hold and body requests
// 43 with nothing pending, then 8 * ((c & 12) + 8 * ((c & 2) + 4 * (c & 1))) + 10 dual updates
// for the owning connection id c (+0x78), each the secondary's 43 then the primary's, their root
// output dropped (the body stays put). An org0 or item row's body runs none of it, nor does a
// body holding no animation channel.
// [orig: PlayerClass_InitEntity @0x4B1060 - @0x4B115B, @0x4B1161, @0x4B1176, the channel test
//  @0x4B116D / @0x4B11D0, the count @0x4B11D2..0x4B11E8, the loop @0x4B11F3..0x4B1225 (the
//  AnimMap_UpdateDualChannels call @0x4B11FB into a stack buffer)]
void test_the_plyr_class_init_warms_up_its_channels() {
	const uint32_t owners[] = {0, 1, 2, 3, 4, 12, 15};
	const int32_t steps[] = {10, 266, 138, 394, 42, 106, 490};
	for (size_t i = 0; i < 7; ++i) {
		Clips clips;
		Rig r;
		r.world->ai.root_motion = &clips;
		w::PlayerSpawn s;
		s.player_class = kPlyr;
		s.net_id = r.next_net_id++;
		s.owner_connection_id = owners[i];
		s.equipped_adm_index = kEquipped;
		w::Entity *body = r.bind(w::spawn_remote_player(*r.world, s));
		CHECK(body != nullptr);
		if (body == nullptr) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		const int32_t z = body->position.z.q16;
		w::player_class_init_animation(*r.world, *body);
		const w::AiEntity *ae = r.world->ai.for_handle(body->handle);
		CHECK(ae != nullptr);
		if (ae != nullptr) {
			CHECK(ae->inf.anim_state == w::anim_state::kIdle);
			CHECK(ae->inf.wpn_state == w::anim_state::kIdle);
			CHECK(ae->inf.anim_pending == 0);
			CHECK(ae->inf.clip_phase == steps[i]);
			CHECK(ae->inf.wpn_clip_phase == steps[i]);
		}
		CHECK(body->position.z.q16 == z);
		CHECK(body->net_anim_phase == std::min(steps[i], int32_t{255}));
		std::vector<int> targets;
		for (const int state : clips.calls)
			if (state != w::anim_state::kReset) targets.push_back(state);
		CHECK(targets.size() == static_cast<size_t>(2 * steps[i]));
		CHECK(std::all_of(targets.begin(), targets.end(),
				[](int state) { return state == w::anim_state::kIdle; }));
		r.world->ai.root_motion = nullptr;
	}
	for (const uint8_t player_class : {kOrg, kBrrl, kPlyr}) {
		Clips clips;
		Rig r;
		// The plyr case holds no channel: no source.
		if (player_class != kPlyr) r.world->ai.root_motion = &clips;
		w::Entity *body = r.spawn_remote(player_class);
		CHECK(body != nullptr);
		if (body == nullptr) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		w::player_class_init_animation(*r.world, *body);
		const w::AiEntity *ae = r.world->ai.for_handle(body->handle);
		CHECK(ae != nullptr && ae->inf.clip_phase == 0 && ae->inf.wpn_clip_phase == 0);
		CHECK(clips.calls.empty());
		r.world->ai.root_motion = nullptr;
	}
}


// The placement's warm-up counts by the body's DcbId (+0x7C): 10000 + the slot byte on a plyr
// row (its class init's stamp), the spawn memset's 0 on an org0 or item row (their inits write
// none); then one more update on an unparented body.
// [orig: Entity_WarmUpOrganicAnimation @0x4B8B20 - `mov eax, [esi+7Ch]` @0x4B8B3F, the loop
//  @0x4B8B3F..0x4B8BA3, the final update @0x4B8BC1; PlayerClass_InitEntity @0x4B1060 -
//  @0x4B1149..0x4B1173; Entity_SpawnFromAnimSlotProperty @0x43C390 - the memset @0x43C3FE]
void test_the_placement_warm_up_counts_by_the_dcb_id() {
	struct Case {
		uint8_t player_class;
		uint8_t slot;
		int32_t updates;
	};
	for (const Case c : {Case{kPlyr, 0, 11}, Case{kPlyr, 1, 267}, Case{kPlyr, 3, 395},
				Case{kOrg, 1, 11}, Case{kBrrl, 3, 11}}) {
		Clips clips;
		Rig r;
		w::Entity *body = r.spawn_remote(c.player_class);
		CHECK(body != nullptr);
		if (body == nullptr) continue;
		body->player_slot_id = c.slot;
		r.world->ai.root_motion = &clips;
		w::warm_up_player_body(*r.world, *body);
		const w::AiEntity *ae = r.world->ai.for_handle(body->handle);
		CHECK(ae != nullptr && ae->inf.clip_phase == c.updates);
		r.world->ai.root_motion = nullptr;
	}
}

// The entity update calls a body's +0x1C4, its bound row's move function (def+0x158, the spawn's
// copy): the plyr row's org2 runs the player-body update (its MoveOrder decoded at the head, the
// savedLivePose stamped), the org0 class row's org1 the AI update (the stamp, no MoveOrder read),
// and a row naming none binds the null row, whose stub the walk skips: nothing runs. A World with
// no items rows binds a player body the seed's row (org2) and any other body org1.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the read @0x4C2460, the nullsub_2 compare
//  @0x4C2466..0x4C246B, the null test @0x4C246F, the call @0x4C2472;
//  Entity_SpawnFromAnimSlotProperty @0x43C390 - @0x43C440, the store @0x43C453;
//  Entity_UpdateInfantryPlayerBody @0x4B40E0 - MoveOrder @0x4B414B, the stamp
//  @0x4B4187..0x4B419C; Entity_UpdateInfantryAI @0x4B9910 - the stamp @0x4B9A53..0x4B9A6E;
//  both stamps store +0x80 / +0x84 / +0x88 alone]
void test_the_update_follows_the_rows_move_function() {
	struct Case {
		uint8_t player_class;
		w::PhysicsClass row;
		bool ran;
		bool moving;
	};
	for (const Case c : {Case{kPlyr, w::PhysicsClass::Org2, true, true},
				Case{kOrg, w::PhysicsClass::Org1, true, false},
				Case{kNull, w::PhysicsClass::Null, false, false}}) {
		Clips clips;
		Rig r;
		r.world->ai.root_motion = &clips;
		w::Entity *body = r.spawn_remote(c.player_class);
		w::AiEntity *ae = body != nullptr ? r.world->ai.for_handle(body->handle) : nullptr;
		CHECK(ae != nullptr);
		if (ae == nullptr) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		CHECK(w::body_move_row(*r.world, *ae, body) == c.row);
		// The join's placement clears the add's hidden bit [orig: Server_OnPlayerJoin
		//  @0x51A7DA]; the uplink's MoveOrder says moving.
		body->flags &= ~w::kEntityFlagCarried;
		body->engine_flags &= ~w::kEntityFlagCarried;
		body->net_move_input = 0x08;
		body->saved_live_valid = false;
		// The stamp is the three Position words alone: the +0x8C..+0x94 words (a torso
		// hit's roll on +0x94) survive the update.
		body->saved_live_yaw = 0x01000000;
		body->saved_live_pitch = 0x00200000;
		body->saved_live_roll = 0x05B05B00;
		const int32_t entry[3] = {body->position.x.q16, body->position.y.q16,
				body->position.z.q16};
		r.world->ai.update_organic(*ae, *r.world, 1);
		const w::Entity *after = r.world->registry.get(ae->handle);
		CHECK(after != nullptr && after->saved_live_valid == c.ran);
		if (after != nullptr) {
			CHECK(after->saved_live_yaw == 0x01000000);
			CHECK(after->saved_live_pitch == 0x00200000);
			CHECK(after->saved_live_roll == 0x05B05B00);
			if (c.ran)
				for (int axis = 0; axis < 3; ++axis)
					CHECK(after->saved_live_pos[axis] == entry[axis]);
		}
		CHECK(ae->inf.player_moving == c.moving);
		r.world->ai.root_motion = nullptr;
	}
	// A World with no items rows.
	std::unique_ptr<w::World> bare = std::make_unique<w::World>();
	w::AiEntity e{};
	w::Entity person{};
	CHECK(w::body_move_row(*bare, e, &person) == w::PhysicsClass::Org1);
	person.flags = w::kEntityFlagPlayer;
	CHECK(w::body_move_row(*bare, e, &person) == w::PhysicsClass::Org2);
}

// The same dispatch through the entity update's pool-0 walk: a body on the null row is skipped
// whole (no update, no tail), a body on the org2 row runs its update.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the nullsub_2 compare @0x4C2466..0x4C246B, the
//  null test @0x4C246F to the next slot, the call @0x4C2472]
void test_the_walk_skips_a_null_row_body_whole() {
	struct Case {
		uint8_t player_class;
		bool ran;
	};
	for (const Case c : {Case{kNull, false}, Case{kPlyr, true}}) {
		Clips clips;
		Rig r;
		r.world->ai.root_motion = &clips;
		w::Entity *body = r.spawn_remote(c.player_class);
		CHECK(body != nullptr);
		if (body == nullptr) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		body->flags &= ~w::kEntityFlagCarried;
		body->engine_flags &= ~w::kEntityFlagCarried;
		body->saved_live_valid = false;
		const w::EntityHandle handle = body->handle;
		w::TickContext ctx;
		ctx.world = r.world.get();
		ctx.logic_tick = 1;
		ctx.is_authority = true;
		r.world->logic_tick = 1;
		r.world->update_all_entities(ctx);
		const w::Entity *after = r.world->registry.get(handle);
		CHECK(after != nullptr && after->saved_live_valid == c.ran);
		r.world->ai.root_motion = nullptr;
	}
}

// A body bound to a vehicle or aircraft row runs that row's mover over its own words: each loads
// the brain word (+0x64), which a person's class init never fills, stamps the six savedLivePose
// words from the body's Position and Yaw / Pitch / Roll words and jumps to its matrix-build tail
// (Flags 0x20000), with no AI or player-body update after it; the chld row's mover is a bare
// return (D-NET-NEW-L4-14 (a)).
// [orig: Entity_UpdateVehiclePhysics @0x48AF00 - @0x48AF1A, @0x48AF74..0x48AFA0, @0x48AFA6 ->
//  @0x48D43E..0x48D451; Vehicle_ProcessSelectorZeroGround @0x46E100 - @0x46E119,
//  @0x46E12D..0x46E15F, @0x46E165 -> @0x46F9B2..0x46F9C5; Entity_UpdateTankVehiclePhysics
//  @0x488AB0 - @0x488B56 -> @0x48AEB6..0x48AEC9; Entity_UpdateLightVehiclePhysics @0x483FE0 -
//  @0x484086 -> @0x486A04..0x486A17; Entity_UpdateWatercraftPhysics @0x48D480 - @0x48D4EF ->
//  @0x48EF50..0x48EF63; Vehicle_ProcessSelectorZeroBoat @0x46FA00 - @0x46FA64 ->
//  @0x471670..0x471683; Entity_UpdateAircraftPhysics @0x490310 - @0x490371 ->
//  @0x492753..0x492766; nullsub_82 @0x45D540]
void test_a_vehicle_row_body_runs_its_movers_person_path() {
	struct Case {
		w::PhysicsClass row;
		bool ran;
	};
	using P = w::PhysicsClass;
	for (const Case c : {Case{P::Cveh, true}, Case{P::Ctrn, true}, Case{P::Catv, true},
				Case{P::Ctank, true}, Case{P::Cbike, true}, Case{P::Cbot, true},
				Case{P::Cpln, true}, Case{P::Chel, true}, Case{P::Chld, false}}) {
		Clips clips;
		Rig r;
		r.world->ai.root_motion = &clips;
		w::Entity *body = r.spawn_remote(kPlyr);
		w::AiEntity *ae = body != nullptr ? r.world->ai.for_handle(body->handle) : nullptr;
		CHECK(ae != nullptr && body->item_type_index > 0);
		if (ae == nullptr || body->item_type_index <= 0) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		// The test's own row: the plyr person bound to the case's move function.
		r.world->tables.item_rows[static_cast<size_t>(body->item_type_index)].move_row = c.row;
		CHECK(w::body_move_row(*r.world, *ae, body) == c.row);
		body->flags &= ~(w::kEntityFlagCarried | w::kEntityFlagMatrixBuilt);
		body->engine_flags &= ~(w::kEntityFlagCarried | w::kEntityFlagMatrixBuilt);
		body->net_move_input = 0x08;
		body->heading = 0x12340000;
		body->pitch_bam = 0x00560000;
		body->roll_bam = -0x00780000;
		body->saved_live_valid = false;
		body->saved_live_yaw = 0x01000000;
		body->saved_live_pitch = 0x00200000;
		body->saved_live_roll = 0x05B05B00;
		const int32_t entry[3] = {body->position.x.q16, body->position.y.q16,
				body->position.z.q16};
		r.world->ai.update_organic(*ae, *r.world, 1);
		const w::Entity *after = r.world->registry.get(ae->handle);
		CHECK(after != nullptr && after->saved_live_valid == c.ran);
		if (after != nullptr) {
			const bool matrix = ((after->flags | after->engine_flags) & w::kEntityFlagMatrixBuilt) != 0;
			CHECK(matrix == c.ran);
			CHECK(after->saved_live_yaw == (c.ran ? 0x12340000 : 0x01000000));
			CHECK(after->saved_live_pitch == (c.ran ? 0x00560000 : 0x00200000));
			CHECK(after->saved_live_roll == (c.ran ? -0x00780000 : 0x05B05B00));
			if (c.ran)
				for (int axis = 0; axis < 3; ++axis)
					CHECK(after->saved_live_pos[axis] == entry[axis]);
		}
		CHECK(!ae->inf.player_moving); // no player-body update decoded the MoveOrder
		r.world->ai.root_motion = nullptr;
	}
}

// A body bound to the genx row runs the parent follow over its own words: the spawn stored the
// row's callback as its +0x1C4 (ours: the sweep installs the ride drop motion), which stamps the
// savedLivePose words and moves the body by its +0x28 ground entity's per-tick delta, with no
// player-body update after it.
// [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 - the +0x1C4 store @0x43C453;
//  Entity_UpdateParentTransform @0x4A88B0 - the stamp @0x4A88D5..0x4A8902, the +0x28 read
//  @0x4A88F1; Entity_InterpolateFromParentDelta @0x4A8D60 - the translation @0x4A8E0F..0x4A8E18]
void test_a_genx_row_body_rides_its_ground_entity() {
	Clips clips;
	Rig r;
	r.world->ai.root_motion = &clips;
	r.world->registry.configure_pool(1, 4);
	w::Entity *body = r.spawn_remote(kGenx);
	w::AiEntity *ae = body != nullptr ? r.world->ai.for_handle(body->handle) : nullptr;
	CHECK(ae != nullptr);
	if (ae == nullptr) {
		r.world->ai.root_motion = nullptr;
		return;
	}
	CHECK(w::body_move_row(*r.world, *ae, body) == w::PhysicsClass::Genx);
	CHECK(body->drop_motion == w::DropMotion::Ride);
	// The ground entity moved 3 units along X since its last stamp, its attitude unchanged.
	w::Entity ground_seed;
	ground_seed.alive = true;
	ground_seed.position.set_q16({10 << 16, 20 << 16, 0});
	const w::EntityHandle ground = r.world->registry.spawn(1, ground_seed);
	w::Entity *carrier = r.world->registry.get(ground);
	CHECK(carrier != nullptr);
	if (carrier == nullptr) {
		r.world->ai.root_motion = nullptr;
		return;
	}
	carrier->saved_live_pos[0] = 7 << 16;
	carrier->saved_live_pos[1] = 20 << 16;
	carrier->saved_live_pos[2] = 0;
	carrier->saved_live_yaw = carrier->heading;
	carrier->saved_live_pitch = carrier->pitch_bam;
	carrier->saved_live_roll = carrier->roll_bam;
	carrier->saved_live_valid = true;
	body = r.world->registry.get(ae->handle);
	body->flags &= ~w::kEntityFlagCarried;
	body->engine_flags &= ~w::kEntityFlagCarried;
	body->net_move_input = 0x08;
	body->ground_target = ground;
	body->saved_live_valid = false;
	const int32_t entry[3] = {body->position.x.q16, body->position.y.q16,
			body->position.z.q16};
	r.world->ai.update_organic(*ae, *r.world, 1);
	const w::Entity *after = r.world->registry.get(ae->handle);
	CHECK(after != nullptr && after->saved_live_valid);
	if (after != nullptr) {
		for (int axis = 0; axis < 3; ++axis)
			CHECK(after->saved_live_pos[axis] == entry[axis]);
		CHECK(after->position.x.q16 == entry[0] + (3 << 16));
		CHECK(after->position.y.q16 == entry[1]);
		CHECK(after->position.z.q16 == entry[2]);
		CHECK(after->drop_motion == w::DropMotion::Ride);
	}
	CHECK(!ae->inf.player_moving);
	r.world->ai.root_motion = nullptr;
}

// A body bound to the rokt, hlfr or jvln row runs that row's mover over its +0x2B8 pending
// dword, which the player record tests whole and sends the low byte of: rokt sets the high word
// (+0x2BA) to 1 while it is 0, so the record sends the low byte (0) over the current state;
// hlfr and jvln set bit 0 when the body has no +0x170 link and return while it is set. The
// envs, door, squib (a live body: its death tick 0) and upfx (its 0x4 bit clear) rows write no
// word of the body's (envs's and upfx's loop registration is the next test's). None runs the
// player-body update after it (D-NET-NEW-L4-14 (a)).
// [orig: Entity_UpdateRocketFlag @0x443DD0 - @0x443DD4, @0x443DDE; Entity_UpdateGuidedMissile
//  @0x446690 - @0x4466B3..0x4466CB, @0x4466DA..0x4466DC; Missile_UpdateGuidance @0x446BA0 -
//  @0x446BBD..0x446BD9, @0x446BE8..0x446BEA; the player record @0x4C0CC7 / @0x4C0CD6 /
//  @0x4C0CDE; Entity_ProcessProjectileTravel @0x448D50 - @0x448D5E..0x448D65;
//  Entity_UpdateWaterPhysicsAndEffects @0x4A92E0 - @0x4A92F5; Entity_UpdateEnvSoundEmitter
//  @0x4A8080; Entity_SetDefaultBoneCallbacks @0x4A91D0]
void test_an_item_row_body_runs_its_movers_head() {
	struct Case {
		w::PhysicsClass row;
		bool link;       // the body carries a +0x170 link
		int32_t pending; // the pending dword before the visit
		int32_t after;   // and after it
	};
	using P = w::PhysicsClass;
	for (const Case c : {Case{P::Rokt, false, 0, 0x10000}, Case{P::Rokt, false, 0x20005, 0x20005},
				Case{P::Hlfr, false, 0, 1}, Case{P::Jvln, false, 12, 13},
				Case{P::Hlfr, true, 13, 13}, Case{P::Jvln, true, 13, 13},
				Case{P::Envs, false, 12, 12}, Case{P::Door, false, 12, 12},
				Case{P::Squib, false, 12, 12}, Case{P::Upfx, false, 12, 12}}) {
		Clips clips;
		Rig r;
		r.world->ai.root_motion = &clips;
		const w::Entity *other = r.spawn_remote(kPlyr);
		const w::EntityHandle link = other != nullptr ? other->handle : w::EntityHandle{};
		w::Entity *body = r.spawn_remote(kPlyr);
		w::AiEntity *ae = body != nullptr ? r.world->ai.for_handle(body->handle) : nullptr;
		CHECK(link.valid() && ae != nullptr && body->item_type_index > 0);
		if (!link.valid() || ae == nullptr || body->item_type_index <= 0) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		// The test's own row: the plyr person bound to the case's move function.
		r.world->tables.item_rows[static_cast<size_t>(body->item_type_index)].move_row = c.row;
		CHECK(w::body_move_row(*r.world, *ae, body) == c.row);
		body->flags &= ~(w::kEntityFlagCarried | w::kEntityFlagMatrixBuilt | w::kEntityFlagNVGWorn);
		body->engine_flags &=
				~(w::kEntityFlagCarried | w::kEntityFlagMatrixBuilt | w::kEntityFlagNVGWorn);
		body->primary_occupant = c.link ? link : w::EntityHandle{};
		body->death_tick = 0;
		body->net_move_input = 0x08;
		body->saved_live_valid = false;
		ae->inf.anim_pending = c.pending;
		body->net_anim_pending = c.pending;
		const int32_t entry[3] = {body->position.x.q16, body->position.y.q16,
				body->position.z.q16};
		const uint32_t flags = body->flags | body->engine_flags;
		r.world->ai.update_organic(*ae, *r.world, 1);
		const w::Entity *after = r.world->registry.get(ae->handle);
		CHECK(after != nullptr);
		if (after != nullptr) {
			CHECK(ae->inf.anim_pending == c.after);
			CHECK(after->net_anim_pending == c.after);
			const GameEntitySnapshot s = replication::snapshot_of(*after);
			const uint8_t sent = c.after != 0 ? static_cast<uint8_t>(c.after & 0xFF)
			                                  : after->net_anim_state;
			CHECK(snapshot_anim_byte(s) == sent);
			CHECK(!after->saved_live_valid);
			CHECK((after->flags | after->engine_flags) == flags);
			CHECK(after->position.x.q16 == entry[0]);
			CHECK(after->position.y.q16 == entry[1]);
			CHECK(after->position.z.q16 == entry[2]);
		}
		CHECK(!ae->inf.player_moving); // no player-body update decoded the MoveOrder
		r.world->ai.root_motion = nullptr;
	}
}

// A body bound to the envs row registers its def's regional loop on every visit: the row's mover
// is the emitter itself, with no gate (a dead body's too, off the batch's last tick too). The upfx
// row's tail registers it only on the batch's last tick, for a def with no physics selector and a
// body with neither Flags 0x2 nor 0x4. The registration is the def's: a person's type (3) keeps
// the default 10-tick lifetime, the same loop in every region a full blend.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-0 call @0x4C2472;
//  Entity_UpdateEnvSoundEmitter @0x4A8080 - the def+0x5C switch @0x4A80A0, the 10 @0x4A80BC,
//  the 0xFFFF blend @0x4A819F, the volume @0x4A81C6, the register @0x4A81DE;
//  Entity_UpdateWaterPhysicsAndEffects @0x4A92E0 - @0x4A92E8, @0x4A93C5, @0x4A93CC, @0x4A93D6]
void test_an_envs_or_upfx_row_body_registers_its_loop() {
	struct Case {
		w::PhysicsClass row;
		uint32_t flags;   // the body's Flags bits for the visit
		bool last_tick;   // the batch's last tick
		int32_t physics;  // the def's +0x8DC selector
		bool registered;
	};
	using P = w::PhysicsClass;
	for (const Case c : {Case{P::Envs, 0, true, 0, true}, Case{P::Envs, 0, false, 0, true},
				Case{P::Envs, w::kEntityFlagDead, false, 3, true},
				Case{P::Upfx, 0, true, 0, true}, Case{P::Upfx, 0, false, 0, false},
				Case{P::Upfx, w::kEntityFlagDead, true, 0, false},
				Case{P::Upfx, w::kEntityFlagNVGWorn, true, 0, false},
				Case{P::Upfx, 0, true, 3, false}}) {
		Clips clips;
		Rig r;
		r.world->ai.root_motion = &clips;
		w::Entity *body = r.spawn_remote(kPlyr);
		w::AiEntity *ae = body != nullptr ? r.world->ai.for_handle(body->handle) : nullptr;
		w::ItemDeathTraits *traits =
				body != nullptr ? r.world->tables.item_death_traits.get_mutable(body->item_id) : nullptr;
		CHECK(ae != nullptr && traits != nullptr && body->item_type_index > 0);
		if (ae == nullptr || traits == nullptr || body->item_type_index <= 0) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		r.world->tables.item_rows[static_cast<size_t>(body->item_type_index)].move_row = c.row;
		CHECK(w::body_move_row(*r.world, *ae, body) == c.row);
		for (std::string &loop : traits->regional_loops) loop = "loop";
		traits->physics = c.physics;
		body->flags &= ~(w::kEntityFlagCarried | w::kEntityFlagDead | w::kEntityFlagNVGWorn);
		body->engine_flags &= ~(w::kEntityFlagCarried | w::kEntityFlagDead | w::kEntityFlagNVGWorn);
		body->flags |= c.flags;
		body->engine_flags |= c.flags;
		body->net_move_input = 0x08;
		r.world->rules.last_tick_of_batch = c.last_tick;
		r.world->out.sound_emitters.clear();
		r.world->ai.update_organic(*ae, *r.world, 1);
		const auto &sent = r.world->out.sound_emitters.pending();
		CHECK(sent.size() == (c.registered ? 1u : 0u));
		if (c.registered && sent.size() == 1) {
			CHECK(sent[0].source_handle == body->handle.packed);
			CHECK(sent[0].set_name == "loop");
			CHECK(sent[0].lifetime_ticks == 10);
			CHECK(sent[0].volume_q8_8 == 0xFFFE);
			CHECK(sent[0].pitch_q16 == 0x10000);
		}
		CHECK(!ae->inf.player_moving);
		r.world->ai.root_motion = nullptr;
	}
}

// A body bound to the ele0 row runs the elevator mover over its own words: its lift the +0x1A8
// word (InfantryState::target_heading), its vertical step the motor's +0xA0, its state +0x29C and
// previous state +0x2A0 a player body's 0s. Above 0 it lowers by 0x400 a visit with no change;
// at 0 it stops (the state change: the def's door-open set and its first loop); a body whose
// +0x28 ground entity stands on it raises (state 2, the loop alone). No player-body update follows.
// [orig: Entity_UpdateFallAndLandState @0x4A20E0 - @0x4A2116..0x4A2133, @0x4A2147..0x4A217B,
//  @0x4A2187..0x4A21F2]
void test_an_ele0_row_body_runs_the_elevator_mover() {
	Clips clips;
	Rig r;
	r.world->ai.root_motion = &clips;
	const w::Entity *other = r.spawn_remote(kPlyr);
	const w::EntityHandle rider = other != nullptr ? other->handle : w::EntityHandle{};
	w::Entity *body = r.spawn_remote(kPlyr);
	w::AiEntity *ae = body != nullptr ? r.world->ai.for_handle(body->handle) : nullptr;
	w::ItemDeathTraits *traits =
			body != nullptr ? r.world->tables.item_death_traits.get_mutable(body->item_id) : nullptr;
	CHECK(rider.valid() && ae != nullptr && traits != nullptr && body->item_type_index > 0);
	if (!rider.valid() || ae == nullptr || traits == nullptr || body->item_type_index <= 0) {
		r.world->ai.root_motion = nullptr;
		return;
	}
	r.world->tables.item_rows[static_cast<size_t>(body->item_type_index)].move_row =
			w::PhysicsClass::Ele0;
	CHECK(w::body_move_row(*r.world, *ae, body) == w::PhysicsClass::Ele0);
	traits->regional_loops[0] = "Lift";
	traits->door_open_set = "Clunk";
	body->flags &= ~w::kEntityFlagCarried;
	body->engine_flags &= ~w::kEntityFlagCarried;
	body->net_move_input = 0x08;
	body->ground_target = {};
	body->veh.speed = 0;
	body->script_next_ssn = 0;
	body->saved_live_valid = false;
	body->position.set_q16({5 << 16, 6 << 16, 20 << 16});
	ae->inf.target_heading = 0x800;
	ae->inf.vel[2] = 0x1234;
	r.world->out.slot_sounds.clear();
	r.world->out.sound_emitters.clear();
	// Lowering: 0x800 -> 0x400, still above 0, no change.
	r.world->ai.update_organic(*ae, *r.world, 1);
	CHECK(body->saved_live_valid && body->saved_live_pos[2] == 20 << 16);
	CHECK(ae->inf.target_heading == 0x400 && ae->inf.vel[2] == -0x400);
	CHECK(body->position.z.q16 == (20 << 16) - 0x400);
	CHECK(body->veh.speed == 0 && body->script_next_ssn == 0);
	CHECK(r.world->out.slot_sounds.empty() && r.world->out.sound_emitters.empty());
	// At 0 it stops: the cue and the loop.
	r.world->ai.update_organic(*ae, *r.world, 2);
	CHECK(ae->inf.target_heading == 0 && body->position.z.q16 == (20 << 16) - 0x800);
	CHECK(body->veh.speed == 1 && body->script_next_ssn == 1);
	CHECK(r.world->out.slot_sounds.size() == 1 &&
			std::string(r.world->out.slot_sounds[0].set_name) == "Clunk");
	CHECK(r.world->out.sound_emitters.size() == 1 &&
			r.world->out.sound_emitters[0].set_name == "Lift");
	// A rider: the body's ground entity stands on it, so it raises (the loop alone).
	r.world->registry.get(rider)->ground_target = body->handle;
	body->ground_target = rider;
	r.world->out.slot_sounds.clear();
	r.world->out.sound_emitters.clear();
	r.world->ai.update_organic(*ae, *r.world, 3);
	CHECK(body->veh.speed == 2 && body->script_next_ssn == 2 && ae->inf.vel[2] == 0);
	CHECK(r.world->out.slot_sounds.empty() && r.world->out.sound_emitters.size() == 1);
	r.world->ai.update_organic(*ae, *r.world, 4);
	CHECK(ae->inf.vel[2] == 0x400 && body->position.z.q16 == (20 << 16) - 0x400);
	CHECK(!ae->inf.player_moving);
	r.world->ai.root_motion = nullptr;
}

// A body bound to the arty row zeroes its velocity triple on every visit, then on the session's
// authority sends S2C 0x44 group 6 when its def's network callback is the guided serializer (an
// arty / rokt / stng / hlfr / jvln ai_function): the +0x170 link's handle (0xFFFF with none),
// the +0x78 word, the attach triple (+0x2E4 / +0x2E8 / +0x2EC: the legs' re-plant pair and the
// aim heading). A plyr row's serializer returns -1 for group 6, so its body sends nothing; nor
// does a joiner or a session-less world. No player-body update follows.
// [orig: ArtyClass_UpdateBarrage @0x447140 - @0x44714F..0x447163, @0x4472C9..0x4472D9;
//  NetPacket_SerializeEntityForServer @0x4D6240 - @0x4D62B1, @0x4D62B9..0x4D62F3, @0x4D62FC,
//  @0x4D632C; Entity_SerializeGuidedMissileState @0x447C50 - @0x447DF6..0x447E44;
//  NetPacket_SerializePlayerState @0x4C09C0 - @0x4C09F6, @0x4C20D1]
void test_an_arty_row_body_zeroes_its_velocity_and_sends_group_6() {
	struct Case {
		uint8_t player_class;
		bool link, session, authority;
		bool sent;
	};
	for (const Case c : {Case{kArty, false, true, true, true}, Case{kArty, true, true, true, true},
				Case{kArty, false, true, false, false}, Case{kArty, false, false, true, false},
				Case{kPlyr, false, true, true, false}}) {
		Clips clips;
		Rig r;
		r.world->ai.root_motion = &clips;
		const w::Entity *other = r.spawn_remote(kPlyr);
		const w::EntityHandle link = other != nullptr ? other->handle : w::EntityHandle{};
		w::Entity *body = r.spawn_remote(c.player_class);
		w::AiEntity *ae = body != nullptr ? r.world->ai.for_handle(body->handle) : nullptr;
		CHECK(link.valid() && ae != nullptr && body->item_type_index > 0);
		if (!link.valid() || ae == nullptr || body->item_type_index <= 0) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		// The plyr case: the test's own row, the plyr person bound to the arty move function.
		r.world->tables.item_rows[static_cast<size_t>(body->item_type_index)].move_row =
				w::PhysicsClass::Arty;
		CHECK(w::body_move_row(*r.world, *ae, body) == w::PhysicsClass::Arty);
		CHECK(w::player_item_row(*r.world, *body).guided_record == (c.player_class == kArty));
		body->flags &= ~w::kEntityFlagCarried;
		body->engine_flags &= ~w::kEntityFlagCarried;
		body->net_move_input = 0x08;
		body->owner_connection_id = 5;
		body->primary_occupant = c.link ? link : w::EntityHandle{};
		ae->inf.vel[0] = 0x400;
		ae->inf.vel[1] = -0x300;
		ae->inf.vel[2] = 0x200;
		ae->inf.leg_target[0] = 0x11110000;
		ae->inf.leg_target[1] = 0x22220000;
		ae->inf.aim_heading = 0x33330000;
		r.world->rules.mp_session = c.session;
		r.world->rules.logic_authority = c.authority;
		r.world->out.entity_events.clear();
		r.world->ai.update_organic(*ae, *r.world, 1);
		for (int axis = 0; axis < 3; ++axis) CHECK(ae->inf.vel[axis] == 0);
		CHECK(r.world->out.entity_events.size() == (c.sent ? 1u : 0u));
		if (c.sent && r.world->out.entity_events.size() == 1) {
			const auto *send = std::get_if<w::GuidedUpdateEvent>(&r.world->out.entity_events[0]);
			CHECK(send != nullptr);
			if (send != nullptr) {
				CHECK(send->shooter == (c.link ? link.packed : 0xFFFF));
				CHECK(send->net_id == 5);
				CHECK(send->group == 6);
				CHECK(send->state.saved[0] == 0x11110000);
				CHECK(send->state.saved[1] == 0x22220000);
				CHECK(send->state.saved[2] == 0x33330000);
			}
		}
		CHECK(!ae->inf.player_moving);
		r.world->ai.root_motion = nullptr;
	}
}

// A body bound to the psec row runs the physics step over its own words: the savedLivePose
// stamp, then its velocity triple (+0x98 / +0x9C / +0xA0, the motor's) against the terrain
// plus a quarter unit (no terrain here: 0x4000). Airborne it moves by the triple, falls 167,
// turns 0x800000 and pitches 0x1000000 (its +0x270 word, a person's 0, is not 17); on the
// ground it lands there, the horizontal pair quartered and the fall reflected at a quarter.
// Either way the Flags 0x20000 bit clears, and no player-body update follows.
// [orig: Entity_UpdatePhysicsStep @0x53BE10 - the stamp @0x53BE1E..0x53BE4D, the ground
//  compare @0x53BE6D, the landing @0x53BED7..0x53BF09, the flight @0x53BE75..0x53BECF]
void test_a_psec_row_body_runs_the_physics_step() {
	struct Case {
		int32_t z;
		int32_t vel[3];
		int32_t pos_after[3]; // relative to the entry X / Y, absolute Z
		int32_t vel_after[3];
		bool airborne;
	};
	for (const Case c : {Case{10 << 16, {0x400, -0x400, 0x300},
				{0x400, -0x400, (10 << 16) + 0x300}, {0x400, -0x400, 0x300 - 167}, true},
				Case{0x1000, {0x400, -0x400, -0x2000}, {0, 0, 0x4000}, {0x100, -0x100, 0x800},
				false}}) {
		Clips clips;
		Rig r;
		r.world->ai.root_motion = &clips;
		w::Entity *body = r.spawn_remote(kPlyr);
		w::AiEntity *ae = body != nullptr ? r.world->ai.for_handle(body->handle) : nullptr;
		CHECK(ae != nullptr && body->item_type_index > 0);
		if (ae == nullptr || body->item_type_index <= 0) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		r.world->tables.item_rows[static_cast<size_t>(body->item_type_index)].move_row =
				w::PhysicsClass::Psec;
		CHECK(w::body_move_row(*r.world, *ae, body) == w::PhysicsClass::Psec);
		body->flags &= ~w::kEntityFlagCarried;
		body->engine_flags &= ~w::kEntityFlagCarried;
		body->engine_flags |= w::kEntityFlagMatrixBuilt;
		body->net_move_input = 0x08;
		body->position.set_q16({5 << 16, 6 << 16, c.z});
		body->heading = 0x12340000;
		body->pitch_bam = 0x00560000;
		body->roll_bam = -0x00780000;
		body->saved_live_valid = false;
		for (int i = 0; i < 3; ++i) ae->inf.vel[i] = c.vel[i];
		r.world->ai.update_organic(*ae, *r.world, 1);
		const w::Entity *after = r.world->registry.get(ae->handle);
		CHECK(after != nullptr);
		if (after != nullptr) {
			CHECK(after->saved_live_valid);
			CHECK(after->saved_live_pos[0] == 5 << 16);
			CHECK(after->saved_live_pos[1] == 6 << 16);
			CHECK(after->saved_live_pos[2] == c.z);
			CHECK(after->saved_live_yaw == 0x12340000);
			CHECK(after->saved_live_pitch == 0x00560000);
			CHECK(after->saved_live_roll == -0x00780000);
			CHECK(after->position.x.q16 == (5 << 16) + c.pos_after[0]);
			CHECK(after->position.y.q16 == (6 << 16) + c.pos_after[1]);
			CHECK(after->position.z.q16 == c.pos_after[2]);
			CHECK(after->heading == (c.airborne ? 0x12340000 + 0x800000 : 0x12340000));
			CHECK(after->pitch_bam == (c.airborne ? 0x00560000 + 0x1000000 : 0x00560000));
			CHECK((after->engine_flags & w::kEntityFlagMatrixBuilt) == 0);
		}
		for (int i = 0; i < 3; ++i) CHECK(ae->inf.vel[i] == c.vel_after[i]);
		CHECK(!ae->inf.player_moving);
		r.world->ai.root_motion = nullptr;
	}
}

// The spawn's class init by the bound row (def+0x148): an org0 row's player body takes
// Entity_InitOrganicAI, its legs as a placed person's: the anim pick (the player body's AI slot
// reads zeros: no route, idle 43; the PERMANENT Q8-1 (a) reading of a slot retail would fault
// on), the warm-up by its DcbId (+0x7C, the spawn memset's 0: 10 dual updates and the final one
// for an unparented body) and the aim anchor 3 units along the Yaw word off savedLivePose; the
// plyr row's PlayerClass_InitEntity warms up by +0x78 alone (10 updates for connection 0) and
// stores no aim anchor; an item row's body runs neither.
// [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 - the init @0x43C4F0..0x43C4FE;
//  g_EntityClassEventCallbackTable @0x813000 - org0 @0x813018 (its init 0x4BFCC0), plyr
//  @0x813048 (0x4B1060); Entity_InitOrganicAI @0x4BFCC0 - the route test @0x4BFFA0, the
//  reserved routes @0x4BFFF9 / @0x4C000C, the warm-up call @0x4C01C8, the aim anchor
//  @0x4C01CD..0x4C024E; Entity_WarmUpOrganicAnimation @0x4B8B20 - @0x4B8B3F, the final
//  update @0x4B8BC1]
void test_an_org_rows_player_body_takes_the_organic_init() {
	struct Case {
		uint8_t player_class;
		int32_t updates;
		bool anchored;
	};
	for (const Case c : {Case{kOrg, 11, true}, Case{kPlyr, 10, false}, Case{kBrrl, 0, false}}) {
		Clips clips;
		Rig r;
		r.world->ai.root_motion = &clips;
		w::Entity *body = r.spawn_remote(c.player_class);
		CHECK(body != nullptr);
		if (body == nullptr) {
			r.world->ai.root_motion = nullptr;
			continue;
		}
		w::player_body_class_init(*r.world, *body);
		const w::AiEntity *ae = r.world->ai.for_handle(body->handle);
		CHECK(ae != nullptr);
		if (ae != nullptr) {
			CHECK(ae->inf.clip_phase == c.updates);
			if (c.updates != 0) CHECK(ae->inf.anim_state == w::anim_state::kIdle);
			const bool anchored = ae->inf.aim_point[0] != 0 || ae->inf.aim_point[1] != 0;
			CHECK(anchored == c.anchored);
		}
		r.world->ai.root_motion = nullptr;
	}
}

// A body's DcbId (+0x7C), which the org1 update's stagger key and its think's aim seed read:
// a player body's is its class init's word, 10000 + its slot on a plyr row (ours' AiEntity
// keeps the player's net id), 0 on an org row (the organic init writes none); a placed
// person's is its record's.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the key @0x4B9948..0x4B9953, the aim seed
//  @0x4BAA4B..0x4BAA59; PlayerClass_InitEntity @0x4B1060 - @0x4B1149..0x4B1173;
//  Entity_SpawnFromAnimSlotProperty @0x43C390 - the memset @0x43C3FE]
void test_a_player_bodys_dcb_id_is_its_class_inits() {
	Rig r;
	w::Entity *plyr = r.spawn_remote(kPlyr);
	w::Entity *org = r.spawn_remote(kOrg);
	CHECK(plyr != nullptr && org != nullptr);
	if (plyr == nullptr || org == nullptr) return;
	plyr->player_slot_id = 3;
	org->player_slot_id = 3;
	const w::AiEntity *plyr_ai = r.world->ai.for_handle(plyr->handle);
	const w::AiEntity *org_ai = r.world->ai.for_handle(org->handle);
	CHECK(plyr_ai != nullptr && org_ai != nullptr);
	if (plyr_ai == nullptr || org_ai == nullptr) return;
	CHECK(w::body_dcb_id(*r.world, *plyr_ai, plyr) == 10003);
	CHECK(plyr_ai->net_id != 10003);
	CHECK(w::body_dcb_id(*r.world, *org_ai, org) == 0);
	// The org row's key steps by the tick alone.
	CHECK(w::infantry_org1_key(16, w::body_dcb_id(*r.world, *org_ai, org)) == 16u);
	// A placed person: its record's id.
	w::AiEntity npc{};
	npc.net_id = 77;
	w::Entity person{};
	CHECK(w::body_dcb_id(*r.world, npc, &person) == 77);
}

} // namespace

int main() {
	test_the_tag_names_its_row();
	test_the_spawn_runs_its_rows_init();
	test_the_think_runs_its_rows_callback();
	test_the_barrel_rows_dying_body();
	test_a_kill_event_runs_its_rows_callback();
	test_the_projectile_kill_runs_ahead_of_the_subtraction();
	test_a_knife_runs_its_rows_callback();
	test_a_round_hit_runs_its_rows_callback();
	test_a_round_hit_runs_the_callbacks_tails();
	test_the_player_ssn_follows_the_plyr_class_init();
	test_the_barrel_rows_dying_flag_reaches_the_uplink_gate();
	test_the_nade_row_has_no_callback();
	test_a_device_rows_body_runs_the_device_think();
	test_the_plyr_class_init_warms_up_its_channels();
	test_the_placement_warm_up_counts_by_the_dcb_id();
	test_the_update_follows_the_rows_move_function();
	test_the_walk_skips_a_null_row_body_whole();
	test_a_vehicle_row_body_runs_its_movers_person_path();
	test_a_genx_row_body_rides_its_ground_entity();
	test_an_item_row_body_runs_its_movers_head();
	test_an_envs_or_upfx_row_body_registers_its_loop();
	test_an_arty_row_body_zeroes_its_velocity_and_sends_group_6();
	test_an_ele0_row_body_runs_the_elevator_mover();
	test_a_psec_row_body_runs_the_physics_step();
	test_an_org_rows_player_body_takes_the_organic_init();
	test_a_player_bodys_dcb_id_is_its_class_inits();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_player_body_class\n");
	return 0;
}
